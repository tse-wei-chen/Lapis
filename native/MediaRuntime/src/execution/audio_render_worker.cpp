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

} // namespace

AudioRenderWorker::AudioRenderWorker(std::unique_ptr<IAudioOutput> output)
    : output_(std::move(output)) {
    if (output_ == nullptr) {
        throw std::invalid_argument("AudioRenderWorker requires an output.");
    }
}

void AudioRenderWorker::start(std::shared_ptr<AudioFrameStream> input) {
    if (input == nullptr) {
        throw std::invalid_argument("AudioRenderWorker requires an input stream.");
    }

    std::scoped_lock lock(control_mutex_);
    if (state_.load(std::memory_order_acquire) == AudioRenderWorkerState::Running) {
        throw std::logic_error("AudioRenderWorker is already running.");
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    submitted_frames_.store(0, std::memory_order_relaxed);
    submitted_sample_frames_.store(0, std::memory_order_relaxed);
    stale_epoch_drops_.store(0, std::memory_order_relaxed);
    backpressure_waits_.store(0, std::memory_order_relaxed);
    output_failures_.store(0, std::memory_order_relaxed);
    last_open_status_.store(AudioOutputOpenStatus::Ready, std::memory_order_release);
    last_submit_status_.store(AudioOutputStatus::Accepted, std::memory_order_release);
    last_wait_status_.store(AudioOutputWaitStatus::Ready, std::memory_order_release);
    state_.store(AudioRenderWorkerState::Running, std::memory_order_release);

    try {
        worker_ = std::jthread(
            [this, input = std::move(input)](std::stop_token stop_token) {
                run(stop_token, input);
            });
    } catch (...) {
        state_.store(AudioRenderWorkerState::Faulted, std::memory_order_release);
        throw;
    }
}

void AudioRenderWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }
    worker_.request_stop();
    worker_.join();
}

AudioRenderStats AudioRenderWorker::stats() const noexcept {
    return {
        submitted_frames_.load(std::memory_order_relaxed),
        submitted_sample_frames_.load(std::memory_order_relaxed),
        stale_epoch_drops_.load(std::memory_order_relaxed),
        backpressure_waits_.load(std::memory_order_relaxed),
        output_failures_.load(std::memory_order_relaxed)
    };
}

void AudioRenderWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<AudioFrameStream>& input) noexcept {
    auto final_state = AudioRenderWorkerState::Stopped;
    bool output_opened = false;
    std::optional<AudioFormat> active_format;

    try {
        bool finished = false;
        while (!finished && !stop_token.stop_requested()) {
            auto read_result = input->pop(stop_token);
            if (read_result.status == QueueReadStatus::Stopped) {
                break;
            }
            if (read_result.status == QueueReadStatus::Closed) {
                if (!output_opened) {
                    final_state = AudioRenderWorkerState::Drained;
                    break;
                }

                const auto wait_status = output_->wait_until_drained(stop_token);
                last_wait_status_.store(wait_status, std::memory_order_release);
                if (wait_status == AudioOutputWaitStatus::Ready) {
                    final_state = AudioRenderWorkerState::Drained;
                } else {
                    final_state = state_from_wait_status(wait_status);
                    if (wait_status != AudioOutputWaitStatus::Stopped) {
                        output_failures_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                break;
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
            } else if (!same_audio_format(*active_format, format)) {
                output_failures_.fetch_add(1, std::memory_order_relaxed);
                final_state = AudioRenderWorkerState::UnsupportedFormat;
                break;
            }

            for (;;) {
                if (stop_token.stop_requested()) {
                    finished = true;
                    break;
                }

                const auto submit_status = output_->try_submit(frame);
                last_submit_status_.store(submit_status, std::memory_order_release);
                if (submit_status == AudioOutputStatus::Accepted) {
                    submitted_frames_.fetch_add(1, std::memory_order_relaxed);
                    submitted_sample_frames_.fetch_add(
                        frame.buffer->sample_frame_count(),
                        std::memory_order_relaxed);
                    break;
                }
                if (submit_status != AudioOutputStatus::Backpressure) {
                    output_failures_.fetch_add(1, std::memory_order_relaxed);
                    final_state = state_from_submit_status(submit_status);
                    finished = true;
                    break;
                }

                backpressure_waits_.fetch_add(1, std::memory_order_relaxed);
                const auto wait_status = output_->wait_until_writable(stop_token);
                last_wait_status_.store(wait_status, std::memory_order_release);
                if (wait_status == AudioOutputWaitStatus::Ready) {
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

    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
