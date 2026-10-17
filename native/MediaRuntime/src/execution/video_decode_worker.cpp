#include "execution/video_decode_worker.hpp"

#include "session/playback_media_session.hpp"

#include <stdexcept>
#include <utility>

namespace lapis::media {

VideoDecodeWorker::VideoDecodeWorker(
    PlaybackMediaSession& media,
    std::size_t queue_capacity)
    : media_(media), queue_capacity_(queue_capacity) {
    if (queue_capacity_ == 0) {
        throw std::invalid_argument(
            "VideoDecodeWorker queue capacity must be greater than zero.");
    }
}

std::shared_ptr<VideoFrameStream> VideoDecodeWorker::start(
    PlaybackEpoch epoch) {
    std::scoped_lock lock(control_mutex_);

    if (state_.load(std::memory_order_acquire) ==
        VideoDecodeWorkerState::Running) {
        throw std::logic_error("VideoDecodeWorker is already running.");
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    auto queue = std::make_shared<BoundedQueue<TimedVideoFrame>>(
        queue_capacity_);
    auto stream = std::shared_ptr<VideoFrameStream>(
        new VideoFrameStream(epoch, queue));

    state_.store(VideoDecodeWorkerState::Running, std::memory_order_release);
    try {
        worker_ = std::jthread(
            [this, stream](std::stop_token stop_token) {
                run(stop_token, stream);
            });
    } catch (...) {
        state_.store(VideoDecodeWorkerState::Faulted, std::memory_order_release);
        try {
            queue->close();
        } catch (...) {
            // Preserve the worker-start failure as the primary exception.
        }
        throw;
    }

    return stream;
}

void VideoDecodeWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    worker_.join();
}

void VideoDecodeWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<VideoFrameStream>& stream) noexcept {
    auto final_state = VideoDecodeWorkerState::Stopped;

    try {
        while (!stop_token.stop_requested()) {
            auto frame = media_.decode_next_video_frame(stop_token);
            if (stop_token.stop_requested()) {
                break;
            }

            if (!frame.has_value()) {
                final_state = VideoDecodeWorkerState::Drained;
                break;
            }

            const auto timestamp = frame->timestamp;
            TimedVideoFrame timed_frame{
                stream->epoch_,
                timestamp,
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
        final_state = VideoDecodeWorkerState::Faulted;
    }

    try {
        stream->queue_->close();
    } catch (...) {
        final_state = VideoDecodeWorkerState::Faulted;
    }
    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
