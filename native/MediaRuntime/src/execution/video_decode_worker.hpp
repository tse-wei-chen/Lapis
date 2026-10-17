#pragma once

#include "core/bounded_queue.hpp"
#include "core/playback_epoch.hpp"
#include "core/timed_payload.hpp"
#include "video/video_frame.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

class PlaybackMediaSession;
class VideoDecodeWorker;

using TimedVideoFrame = TimedPayload<VideoFrame>;

class VideoFrameStream final {
public:
    [[nodiscard]] PlaybackEpoch epoch() const noexcept {
        return epoch_;
    }

    [[nodiscard]] QueueReadResult<TimedVideoFrame> pop(
        std::stop_token stop_token = {}) {
        return queue_->pop(stop_token);
    }

    [[nodiscard]] QueueReadResult<TimedVideoFrame> try_pop() {
        return queue_->try_pop();
    }

    [[nodiscard]] std::size_t size() const {
        return queue_->size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return queue_->capacity();
    }

private:
    friend class VideoDecodeWorker;

    VideoFrameStream(
        PlaybackEpoch epoch,
        std::shared_ptr<BoundedQueue<TimedVideoFrame>> queue)
        : epoch_(epoch), queue_(std::move(queue)) {}

    PlaybackEpoch epoch_;
    std::shared_ptr<BoundedQueue<TimedVideoFrame>> queue_;
};

enum class VideoDecodeWorkerState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    Faulted
};

class VideoDecodeWorker final {
public:
    explicit VideoDecodeWorker(
        PlaybackMediaSession& media,
        std::size_t queue_capacity = 8);
    ~VideoDecodeWorker() = default;

    VideoDecodeWorker(const VideoDecodeWorker&) = delete;
    VideoDecodeWorker& operator=(const VideoDecodeWorker&) = delete;

    [[nodiscard]] std::shared_ptr<VideoFrameStream> start(PlaybackEpoch epoch);
    void stop();

    [[nodiscard]] VideoDecodeWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<VideoFrameStream>& stream) noexcept;

    PlaybackMediaSession& media_;
    const std::size_t queue_capacity_;
    std::atomic<VideoDecodeWorkerState> state_{VideoDecodeWorkerState::Idle};
    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
