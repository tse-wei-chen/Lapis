#pragma once

#include "execution/video_decode_worker.hpp"
#include "video/video_processor.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

enum class VideoProcessWorkerState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    UnsupportedInput,
    Faulted
};

class VideoProcessWorker final {
public:
    VideoProcessWorker(
        std::unique_ptr<IVideoFrameProcessor> processor,
        std::size_t output_capacity = 4);
    ~VideoProcessWorker() = default;

    VideoProcessWorker(const VideoProcessWorker&) = delete;
    VideoProcessWorker& operator=(const VideoProcessWorker&) = delete;

    [[nodiscard]] std::shared_ptr<VideoFrameStream> start(
        std::shared_ptr<VideoFrameStream> input);
    void stop();

    [[nodiscard]] VideoProcessWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] VideoProcessStatus last_status() const noexcept {
        return last_status_.load(std::memory_order_acquire);
    }

private:
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<VideoFrameStream>& input,
        const std::shared_ptr<VideoFrameStream>& output) noexcept;

    std::unique_ptr<IVideoFrameProcessor> processor_;
    const std::size_t output_capacity_;
    std::atomic<VideoProcessWorkerState> state_{VideoProcessWorkerState::Idle};
    std::atomic<VideoProcessStatus> last_status_{VideoProcessStatus::Processed};
    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
