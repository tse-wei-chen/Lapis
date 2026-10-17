#include "execution/audio_render_worker.hpp"

#include "audio/audio_format_validation.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace lapis::media {
namespace {

bool same_audio_format(const AudioFormat& left, const AudioFormat& right) {
    return left.sample_format == right.sample_format &&
        left.packing == right.packing &&
        left.sample_rate == right.sample_rate &&
        left.channels == right.channels;
}

bool active_state(AudioRenderWorkerState state) noexcept {
    return state == AudioRenderWorkerState::Running ||
        state == AudioRenderWorkerState::Paused;
}

bool terminal_state(AudioRenderWorkerState state) noexcept {
    return state == AudioRenderWorkerState::Drained ||
        state == AudioRenderWorkerState::Stopped ||
        state == AudioRenderWorkerState::UnsupportedFormat ||
        state == AudioRenderWorkerState::OutputUnavailable ||
        state == AudioRenderWorkerState::Faulted;
}

AudioRenderWorkerState state_from_open_status(AudioOutputOpenStatus status) {
    switch (status) {
        case AudioOutputOpenStatus::UnsupportedFormat:
            return AudioRenderWorkerState::UnsupportedFormat;
        case AudioOutputOpenStatus::DeviceLost:
            return AudioRenderWorkerState::OutputUnavailable;
        case AudioOutputOpenStatus::Faulted:
            return AudioRenderWorkerState::Faulted;
        case AudioOutputOpenStatus::Ready:
        default:
            return AudioRenderWorkerState::Running;
    }
}

AudioRenderWorkerState state_from_submit_status(AudioOutputStatus status) {
    switch (status) {
        case AudioOutputStatus::UnsupportedFormat:
            return AudioRenderWorkerState::UnsupportedFormat;
        case AudioOutputStatus::DeviceLost:
            return AudioRenderWorkerState::OutputUnavailable;
        case AudioOutputStatus::Faulted:
            return AudioRenderWorkerState::Faulted;
        case AudioOutputStatus::Accepted:
        case AudioOutputStatus::Backpressure:
        default:
            return AudioRenderWorkerState::Running;
    }
}

AudioRenderWorkerState state_from_wait_status(AudioOutputWaitStatus status) {
    switch (status) {
        case AudioOutputWaitStatus::Stopped:
            return AudioRenderWorkerState::Stopped;
        case AudioOutputWaitStatus::DeviceLost:
            return AudioRenderWorkerState::OutputUnavailable;
        case AudioOutputWaitStatus::Faulted:
            return AudioRenderWorkerState::Faulted;
        case AudioOutputWaitStatus::Ready:
        default:
            return AudioRenderWorkerState::Running;
    }
}

AudioRenderWorkerState state_from_control_status(AudioOutputControlStatus status) {
    switch (status) {
        case AudioOutputControlStatus::DeviceLost:
            return AudioRenderWorkerState::OutputUnavailable;
        case AudioOutputControlStatus::Faulted:
            return AudioRenderWorkerState::Faulted;
        case AudioOutputControlStatus::Ready:
        default:
            return AudioRenderWorkerState::Running;
    }
}

} // namespace

AudioRenderWorker::AudioRenderWorker(std::unique_ptr<IAudioOutput> output)
    : output_(std::move(output)),
      wait_interrupt_source_(std::make_shared<std::stop_source>()) {
    if (output_ == nullptr) {
        throw std::invalid_argument("AudioRenderWorker requires an output.");
    }
}

AudioRenderWorker::~AudioRenderWorker() {
    stop();
}

void AudioRenderWorker::start(std::shared_ptr<AudioFrameStream> input) {
    if (input == nullptr) {
        throw std::invalid_argument("AudioRenderWorker requires an input stream.");
    }

    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (active_state(state_.load(std::memory_order_acquire))) {
        throw std::logic_error("AudioRenderWorker is already active.");
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    submitted_frames_.store(0, std::memory_order_relaxed);
    submitted_sample_frames_.store(0, std::memory_order_relaxed);
    stale_epoch_drops_.store(0, std::memory_order_relaxed);
    writable_waits_.store(0, std::memory_order_relaxed);
    output_failures_.store(0, std::memory_order_relaxed);
    last_open_status_.store(AudioOutputOpenStatus::Ready, std::memory_order_release);
    last_submit_status_.store(AudioOutputStatus::Accepted, std::memory_order_release);
    last_wait_status_.store(AudioOutputWaitStatus::Ready, std::memory_order_release);
    last_control_status_.store(
        AudioOutputControlStatus::Ready,
        std::memory_order_release);

    {
        std::scoped_lock command_lock(command_mutex_);
        pause_requested_ = false;
        applied_pause_state_ = false;
        applied_control_succeeded_ = true;
        requested_control_revision_ = 0;
        applied_control_revision_ = 0;
        wait_interrupt_source_ = std::make_shared<std::stop_source>();
    }
    state_.store(AudioRenderWorkerState::Running, std::memory_order_release);

    try {
        worker_ = std::jthread(
            [this, input = std::move(input)](std::stop_token stop_token) {
                run(stop_token, input);
            });
    } catch (...) {
        publish_terminal_state(AudioRenderWorkerState::Faulted);
        throw;
    }
}

bool AudioRenderWorker::pause() {
    return request_pause_state(true);
}

bool AudioRenderWorker::resume() {
    return request_pause_state(false);
}

void AudioRenderWorker::stop() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    interrupt_waits();
    command_changed_.notify_all();
    worker_.join();
}

AudioRenderStats AudioRenderWorker::stats() const noexcept {
    return {
        submitted_frames_.load(std::memory_order_relaxed),
        submitted_sample_frames_.load(std::memory_order_relaxed),
        stale_epoch_drops_.load(std::memory_order_relaxed),
        writable_waits_.load(std::memory_order_relaxed),
        output_failures_.load(std::memory_order_relaxed)
    };
}

bool AudioRenderWorker::request_pause_state(bool paused) {
    std::unique_lock lock(command_mutex_);
    const auto current_state = state_.load(std::memory_order_acquire);
    if (paused && current_state == AudioRenderWorkerState::Paused &&
        pause_requested_) {
        return true;
    }
    if (!paused && current_state == AudioRenderWorkerState::Running &&
        !pause_requested_ &&
        applied_control_revision_ == requested_control_revision_) {
        return true;
    }
    if (!active_state(current_state)) {
        return false;
    }

    pause_requested_ = paused;
    const auto revision = ++requested_control_revision_;
    if (wait_interrupt_source_ != nullptr) {
        wait_interrupt_source_->request_stop();
    }
    command_changed_.notify_all();

    command_changed_.wait(lock, [this, revision] {
        return applied_control_revision_ >= revision ||
            terminal_state(state_.load(std::memory_order_acquire));
    });

    return applied_control_revision_ >= revision &&
        applied_control_succeeded_ &&
        applied_pause_state_ == paused;
}

bool AudioRenderWorker::apply_requested_control(
    std::stop_token stop_token,
    bool output_opened,
    AudioRenderWorkerState& final_state) {
    for (;;) {
        if (stop_token.stop_requested()) {
            final_state = AudioRenderWorkerState::Stopped;
            return false;
        }

        bool requested_pause{};
        std::uint64_t requested_revision{};
        {
            std::scoped_lock lock(command_mutex_);
            requested_pause = pause_requested_;
            requested_revision = requested_control_revision_;
        }

        const auto current_state = state_.load(std::memory_order_acquire);
        const bool currently_paused = current_state == AudioRenderWorkerState::Paused;
        if (requested_pause != currently_paused) {
            if (output_opened) {
                const auto control_status = requested_pause
                    ? output_->pause()
                    : output_->resume();
                last_control_status_.store(
                    control_status,
                    std::memory_order_release);
                if (control_status != AudioOutputControlStatus::Ready) {
                    output_failures_.fetch_add(1, std::memory_order_relaxed);
                    final_state = state_from_control_status(control_status);
                    state_.store(final_state, std::memory_order_release);
                    {
                        std::scoped_lock lock(command_mutex_);
                        applied_control_revision_ = requested_revision;
                        applied_pause_state_ = requested_pause;
                        applied_control_succeeded_ = false;
                    }
                    command_changed_.notify_all();
                    return false;
                }
            }

            state_.store(
                requested_pause
                    ? AudioRenderWorkerState::Paused
                    : AudioRenderWorkerState::Running,
                std::memory_order_release);
        }

        {
            std::unique_lock lock(command_mutex_);
            if (requested_revision > applied_control_revision_) {
                applied_control_revision_ = requested_revision;
                applied_pause_state_ = requested_pause;
                applied_control_succeeded_ = true;
            }
            if (wait_interrupt_source_ == nullptr ||
                wait_interrupt_source_->stop_requested()) {
                wait_interrupt_source_ = std::make_shared<std::stop_source>();
            }
            command_changed_.notify_all();

            if (!pause_requested_) {
                return true;
            }

            (void)command_changed_.wait(
                lock,
                stop_token,
                [this] {
                    return !pause_requested_ ||
                        requested_control_revision_ > applied_control_revision_;
                });
        }
    }
}

std::stop_token AudioRenderWorker::current_wait_token() {
    std::scoped_lock lock(command_mutex_);
    if (wait_interrupt_source_ == nullptr) {
        wait_interrupt_source_ = std::make_shared<std::stop_source>();
    }
    // A stopped source is intentionally not replaced here. A control request may
    // have raced between apply_requested_control() and this call. Returning that
    // stopped token forces the pending wait to wake immediately; only the worker
    // may renew the source after it has applied and acknowledged the command.
    return wait_interrupt_source_->get_token();
}

void AudioRenderWorker::interrupt_waits() {
    std::scoped_lock lock(command_mutex_);
    if (wait_interrupt_source_ != nullptr) {
        wait_interrupt_source_->request_stop();
    }
}

void AudioRenderWorker::publish_terminal_state(
    AudioRenderWorkerState state) noexcept {
    state_.store(state, std::memory_order_release);
    command_changed_.notify_all();
}

void AudioRenderWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<AudioFrameStream>& input) noexcept {
    auto final_state = AudioRenderWorkerState::Stopped;
    bool output_opened = false;
    bool input_closed = false;
    std::optional<AudioFormat> active_format;

    try {
        bool finished = false;
        while (!finished && !stop_token.stop_requested()) {
            if (!apply_requested_control(stop_token, output_opened, final_state)) {
                break;
            }

            if (input_closed) {
                const auto wait_status = output_->wait_until_drained(
                    current_wait_token());
                last_wait_status_.store(wait_status, std::memory_order_release);
                if (wait_status == AudioOutputWaitStatus::Ready) {
                    final_state = AudioRenderWorkerState::Drained;
                    break;
                }
                if (wait_status == AudioOutputWaitStatus::Stopped &&
                    !stop_token.stop_requested()) {
                    continue;
                }

                final_state = state_from_wait_status(wait_status);
                if (wait_status != AudioOutputWaitStatus::Stopped) {
                    output_failures_.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }

            auto read_result = input->pop(current_wait_token());
            if (read_result.status == QueueReadStatus::Stopped) {
                if (stop_token.stop_requested()) {
                    break;
                }
                continue;
            }
            if (read_result.status == QueueReadStatus::Closed) {
                if (!output_opened) {
                    final_state = AudioRenderWorkerState::Drained;
                    break;
                }
                input_closed = true;
                continue;
            }
            if (!read_result.has_item()) {
                continue;
            }

            const auto& timed_frame = *read_result.item;
            if (timed_frame.epoch.value() != input->epoch().value()) {
                stale_epoch_drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const auto& frame = timed_frame.payload;
            if (frame.buffer == nullptr || !valid_audio_buffer(*frame.buffer)) {
                output_failures_.fetch_add(1, std::memory_order_relaxed);
                final_state = AudioRenderWorkerState::Faulted;
                break;
            }

            const auto& format = frame.buffer->format();
            if (!output_opened) {
                const auto open_status = output_->open(format);
                last_open_status_.store(open_status, std::memory_order_release);
                if (open_status != AudioOutputOpenStatus::Ready) {
                    output_failures_.fetch_add(1, std::memory_order_relaxed);
                    final_state = state_from_open_status(open_status);
                    break;
                }
                output_opened = true;
                active_format = format;

                // A pause request can race with output open. Apply it before any
                // sample prefix is submitted so logical pause is acknowledged at
                // a precise device boundary.
                if (!apply_requested_control(
                        stop_token,
                        output_opened,
                        final_state)) {
                    break;
                }
            } else if (!same_audio_format(*active_format, format)) {
                output_failures_.fetch_add(1, std::memory_order_relaxed);
                final_state = AudioRenderWorkerState::UnsupportedFormat;
                break;
            }

            const auto total_sample_frames = frame.buffer->sample_frame_count();
            std::uint32_t first_sample_frame{};
            while (first_sample_frame < total_sample_frames) {
                if (!apply_requested_control(
                        stop_token,
                        output_opened,
                        final_state)) {
                    finished = true;
                    break;
                }

                const auto submit_result = output_->try_submit(
                    frame,
                    first_sample_frame);
                last_submit_status_.store(
                    submit_result.status,
                    std::memory_order_release);

                if (submit_result.status == AudioOutputStatus::Accepted) {
                    const auto remaining = total_sample_frames - first_sample_frame;
                    if (submit_result.accepted_sample_frames == 0 ||
                        submit_result.accepted_sample_frames > remaining) {
                        output_failures_.fetch_add(1, std::memory_order_relaxed);
                        final_state = AudioRenderWorkerState::Faulted;
                        finished = true;
                        break;
                    }

                    first_sample_frame += submit_result.accepted_sample_frames;
                    submitted_sample_frames_.fetch_add(
                        submit_result.accepted_sample_frames,
                        std::memory_order_relaxed);
                    if (first_sample_frame == total_sample_frames) {
                        submitted_frames_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }

                    writable_waits_.fetch_add(1, std::memory_order_relaxed);
                    const auto wait_status = output_->wait_until_writable(
                        current_wait_token());
                    last_wait_status_.store(wait_status, std::memory_order_release);
                    if (wait_status == AudioOutputWaitStatus::Ready) {
                        continue;
                    }
                    if (wait_status == AudioOutputWaitStatus::Stopped &&
                        !stop_token.stop_requested()) {
                        continue;
                    }

                    final_state = state_from_wait_status(wait_status);
                    if (wait_status != AudioOutputWaitStatus::Stopped) {
                        output_failures_.fetch_add(1, std::memory_order_relaxed);
                    }
                    finished = true;
                    break;
                }

                if (submit_result.accepted_sample_frames != 0) {
                    output_failures_.fetch_add(1, std::memory_order_relaxed);
                    final_state = AudioRenderWorkerState::Faulted;
                    finished = true;
                    break;
                }
                if (submit_result.status != AudioOutputStatus::Backpressure) {
                    output_failures_.fetch_add(1, std::memory_order_relaxed);
                    final_state = state_from_submit_status(submit_result.status);
                    finished = true;
                    break;
                }

                writable_waits_.fetch_add(1, std::memory_order_relaxed);
                const auto wait_status = output_->wait_until_writable(
                    current_wait_token());
                last_wait_status_.store(wait_status, std::memory_order_release);
                if (wait_status == AudioOutputWaitStatus::Ready) {
                    continue;
                }
                if (wait_status == AudioOutputWaitStatus::Stopped &&
                    !stop_token.stop_requested()) {
                    continue;
                }

                final_state = state_from_wait_status(wait_status);
                if (wait_status != AudioOutputWaitStatus::Stopped) {
                    output_failures_.fetch_add(1, std::memory_order_relaxed);
                }
                finished = true;
                break;
            }
        }
    } catch (...) {
        output_failures_.fetch_add(1, std::memory_order_relaxed);
        final_state = AudioRenderWorkerState::Faulted;
    }

    if (output_opened) {
        if (final_state != AudioRenderWorkerState::Drained) {
            output_->reset();
        }
        output_->close();
    }

    publish_terminal_state(final_state);
}

} // namespace lapis::media
