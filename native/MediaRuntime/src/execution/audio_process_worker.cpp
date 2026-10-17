#include "execution/audio_process_worker.hpp"

#include <stdexcept>
#include <utility>

namespace lapis::media {

AudioProcessWorker::AudioProcessWorker(
    std::unique_ptr<IAudioFrameProcessor> processor,
    std::size_t output_capacity)
    : processor_(std::move(processor)),
      output_capacity_(output_capacity) {
    if (processor_ == nullptr) {
        throw std::invalid_argument(
            "AudioProcessWorker requires a frame processor.");
    }
    if (output_capacity_ == 0) {
        throw std::invalid_argument(
            "AudioProcessWorker output capacity must be greater than zero.");
    }
}

std::shared_ptr<AudioFrameStream> AudioProcessWorker::start(
    std::shared_ptr<AudioFrameStream> input) {
    if (input == nullptr) {
        throw std::invalid_argument(
            "AudioProcessWorker requires an input stream.");
    }

    std::scoped_lock lock(control_mutex_);
    if (state_.load(std::memory_order_acquire) == AudioProcessWorkerState::Running) {
        throw std::logic_error("AudioProcessWorker is already running.");
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    processor_->reset();
    auto queue = std::make_shared<BoundedQueue<TimedAudioFrame>>(output_capacity_);
    auto output = std::shared_ptr<AudioFrameStream>(
        new AudioFrameStream(input->epoch(), queue));

    last_status_.store(AudioProcessStatus::NeedMoreInput, std::memory_order_release);
    state_.store(AudioProcessWorkerState::Running, std::memory_order_release);
    try {
        worker_ = std::jthread(
            [this, input = std::move(input), output](std::stop_token stop_token) {
                run(stop_token, input, output);
            });
    } catch (...) {
        state_.store(AudioProcessWorkerState::Faulted, std::memory_order_release);
        try {
            queue->close();
        } catch (...) {
            // Preserve the thread-start failure as the primary exception.
        }
        throw;
    }
    return output;
}

void AudioProcessWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }
    worker_.request_stop();
    worker_.join();
}

bool AudioProcessWorker::publish(
    AudioProcessResult result,
    PlaybackEpoch epoch,
    std::stop_token stop_token,
    const std::shared_ptr<AudioFrameStream>& output) {
    last_status_.store(result.status, std::memory_order_release);
    if (!result.has_frame()) {
        return result.status == AudioProcessStatus::NeedMoreInput ||
            result.status == AudioProcessStatus::Drained;
    }

    auto frame = std::move(*result.frame);
    TimedAudioFrame timed{
        epoch,
        frame.timestamp,
        std::move(frame)
    };
    return output->queue_->push(std::move(timed), stop_token) ==
        QueueWriteStatus::Pushed;
}

void AudioProcessWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<AudioFrameStream>& input,
    const std::shared_ptr<AudioFrameStream>& output) noexcept {
    auto final_state = AudioProcessWorkerState::Stopped;

    try {
        bool finished = false;
        while (!finished && !stop_token.stop_requested()) {
            auto read_result = input->pop(stop_token);
            if (read_result.status == QueueReadStatus::Stopped) {
                break;
            }
            if (read_result.status == QueueReadStatus::Closed) {
                for (;;) {
                    if (stop_token.stop_requested()) {
                        break;
                    }
                    auto drain_result = processor_->drain();
                    const auto status = drain_result.status;
                    if (status == AudioProcessStatus::Processed) {
                        if (!publish(
                                std::move(drain_result),
                                input->epoch(),
                                stop_token,
                                output)) {
                            finished = true;
                            break;
                        }
                        continue;
                    }

                    last_status_.store(status, std::memory_order_release);
                    if (status == AudioProcessStatus::Drained) {
                        final_state = AudioProcessWorkerState::Drained;
                    } else if (status == AudioProcessStatus::UnsupportedInput) {
                        final_state = AudioProcessWorkerState::UnsupportedInput;
                    } else {
                        final_state = AudioProcessWorkerState::Faulted;
                    }
                    finished = true;
                    break;
                }
                break;
            }
            if (!read_result.has_item()) {
                continue;
            }

            auto input_frame = std::move(*read_result.item);
            auto process_result = processor_->process(input_frame.payload);
            const auto status = process_result.status;
            if (status == AudioProcessStatus::Processed) {
                if (!publish(
                        std::move(process_result),
                        input_frame.epoch,
                        stop_token,
                        output)) {
                    break;
                }
                continue;
            }

            last_status_.store(status, std::memory_order_release);
            if (status == AudioProcessStatus::NeedMoreInput) {
                continue;
            }
            final_state = status == AudioProcessStatus::UnsupportedInput
                ? AudioProcessWorkerState::UnsupportedInput
                : AudioProcessWorkerState::Faulted;
            break;
        }
    } catch (...) {
        last_status_.store(AudioProcessStatus::Failed, std::memory_order_release);
        final_state = AudioProcessWorkerState::Faulted;
    }

    if (final_state != AudioProcessWorkerState::Drained) {
        processor_->reset();
    }
    try {
        output->queue_->close();
    } catch (...) {
        final_state = AudioProcessWorkerState::Faulted;
        last_status_.store(AudioProcessStatus::Failed, std::memory_order_release);
    }
    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
