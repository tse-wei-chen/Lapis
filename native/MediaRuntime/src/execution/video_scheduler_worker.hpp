#pragma once

#include "core/bounded_queue.hpp"
#include "core/playback_clock.hpp"
#include "execution/video_decode_worker.hpp"
#include "video/video_scheduler.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

class VideoSchedulerWorker;

struct ScheduledVideoFrame final {
    TimedVideoFrame frame;
    MediaTime released_clock_position{};
    MediaTime frame_offset{};
};

class VideoPresentationStream final {
public:
    [[nodiscard]] PlaybackEpoch epoch() const noexcept {
        return epoch_;
    }

    [[nodiscard]] QueueReadResult<ScheduledVideoFrame> pop(
        std::stop_token stop_token = {}) {
        return queue_->pop(stop_token);
    }

    [[nodiscard]] QueueReadResult<ScheduledVideoFrame> try_pop() {
        return queue_->try_pop();
    }

    [[nodiscard]] std::size_t size() const {
        return queue_->size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return queue_->capacity();
    }

private:
    friend class VideoSchedulerWorker;

    VideoPresentationStream(
        PlaybackEpoch epoch,
        std::shared_ptr<BoundedQueue<ScheduledVideoFrame>> queue)
        : epoch_(epoch), queue_(std::move(queue)) {}

    PlaybackEpoch epoch_;
    std::shared_ptr<BoundedQueue<ScheduledVideoFrame>> queue_;
};

enum class VideoSchedulerWorkerState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    Faulted
};

struct VideoSchedulingStats final {
    std::uint64_t released_frames{};
    std::uint64_t stale_epoch_drops{};
    std::uint64_t late_drops{};
};

class VideoSchedulerWorker final {
public:
    explicit VideoSchedulerWorker(
        PlaybackClock& clock,
        VideoSchedulerConfig scheduler_config = {},
        std::size_t output_capacity = 2);
    ~VideoSchedulerWorker() = default;

    VideoSchedulerWorker(const VideoSchedulerWorker&) = delete;
    VideoSchedulerWorker& operator=(const VideoSchedulerWorker&) = delete;

    [[nodiscard]] std::shared_ptr<VideoPresentationStream> start(
        std::shared_ptr<VideoFrameStream> input);
    void stop();

    [[nodiscard]] VideoSchedulerWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] VideoSchedulingStats stats() const noexcept;

private:
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<VideoFrameStream>& input,
        const std::shared_ptr<VideoPresentationStream>& output) noexcept;

    PlaybackClock& clock_;
    VideoScheduler scheduler_;
    const std::size_t output_capacity_;
    std::atomic<VideoSchedulerWorkerState> state_{VideoSchedulerWorkerState::Idle};
    std::atomic<std::uint64_t> released_frames_{};
    std::atomic<std::uint64_t> stale_epoch_drops_{};
    std::atomic<std::uint64_t> late_drops_{};
    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
