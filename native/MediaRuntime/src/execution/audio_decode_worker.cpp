#include "execution/audio_decode_worker.hpp"

#include "session/playback_media_session.hpp"

#include <stdexcept>
#include <utility>

namespace lapis::media {

AudioDecodeWorker::AudioDecodeWorker(
    PlaybackMediaSession& media,
    std::size_t queue_capacity)
    : media_(media), queue_capacity_(queue_capacity) {
    if (queue_capacity_ == 0) {
        throw std::invalid_argument(
            "AudioDecodeWorker queue capacity must be greater than zero.");
    }
}

std::shared_ptr<AudioFrameStream> AudioDecodeWorker::start(
    PlaybackEpoch epoch) {
    std::scoped_lock lock(control_mutex_);

    if (state_.load(std::memory_order_acquire) == AudioDecodeWorkerState::Running) {
        throw std::logic_error("AudioDecodeWorker is already running.");
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    auto queue = std::make_shared<BoundedQueue<TimedAudioFrame>>(queue_capacity_);
    auto stream = std::shared_ptr<AudioFrameStream>(
        new AudioFrameStream(epoch, queue));

    state_.store(AudioDecodeWorkerState::Running, std::memory_order_release);
    try {
        worker_ = std::jthread(
            [this, stream](std::stop_token stop_token) {
                run(stop_token, stream);
            });
    } catch (...) {
        state_.store(AudioDecodeWorkerState::Faulted, std::memory_order_release);
        try {
            queue->close();
        } catch (...) {
        }
        throw;
    }

    return stream;
}

void AudioDecodeWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    worker_.join();
}

void AudioDecodeWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<AudioFrameStream>& stream) noexcept {
    auto final_state = AudioDecodeWorkerState::Stopped;

    try {
        while (!stop_token.stop_requested()) {
            auto frame = media_.decode_next_audio_frame(stop_token);
            if (stop_token.stop_requested()) {
                break;
            }
            if (!frame.has_value()) {
                final_state = AudioDecodeWorkerState::Drained;
                break;
            }

            TimedAudioFrame timed_frame{
                stream->epoch_,
                frame->timestamp,
                std::move(*frame)
            };
            const auto result = stream->queue_->push(
                std::move(timed_frame),
                stop_token);
            if (result != QueueWriteStatus::Pushed) {
                break;
            }
        }
    } catch (...) {
        final_state = AudioDecodeWorkerState::Faulted;
    }

    try {
        stream->queue_->close();
    } catch (...) {
        final_state = AudioDecodeWorkerState::Faulted;
    }
    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
