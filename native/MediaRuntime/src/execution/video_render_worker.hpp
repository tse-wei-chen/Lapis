#pragma once

#include "core/playback_clock.hpp"
#include "execution/video_scheduler_worker.hpp"
#include "video/video_presenter.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

enum class VideoRenderWorkerState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    PresenterUnavailable,
    Faulted
};

struct VideoRenderStats final {
    std::uint64_t presented_frames{};
    std::uint64_t stale_epoch_drops{};
    std::uint64_t inactive_clock_drops{};
    std::uint64_t presenter_failures{};
};

class VideoRenderWorker final {
public:
    VideoRenderWorker(
        PlaybackClock& clock,
        std::unique_ptr<IVideoPresenter> presenter);
    ~VideoRenderWorker() = default;

    VideoRenderWorker(const VideoRenderWorker&) = delete;
    VideoRenderWorker& operator=(const VideoRenderWorker&) = delete;

    void start(std::shared_ptr<VideoPresentationStream> input);
    void stop();

    [[nodiscard]] VideoRenderWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] VideoRenderStats stats() const noexcept;

    [[nodiscard]] VideoPresenterBeginStatus last_begin_status() const noexcept {
        return last_begin_status_.load(std::memory_order_acquire);
    }

    [[nodiscard]] VideoPresentStatus last_present_status() const noexcept {
        return last_present_status_.load(std::memory_order_acquire);
    }

private:
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<VideoPresentationStream>& input) noexcept;

    PlaybackClock& clock_;
    std::unique_ptr<IVideoPresenter> presenter_;
    std::atomic<VideoRenderWorkerState> state_{VideoRenderWorkerState::Idle};
    std::atomic<VideoPresenterBeginStatus> last_begin_status_{
        VideoPresenterBeginStatus::Ready
    };
    std::atomic<VideoPresentStatus> last_present_status_{
        VideoPresentStatus::Presented
    };
    std::atomic<std::uint64_t> presented_frames_{};
    std::atomic<std::uint64_t> stale_epoch_drops_{};
    std::atomic<std::uint64_t> inactive_clock_drops_{};
    std::atomic<std::uint64_t> presenter_failures_{};
    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
