#pragma once

#include "core/playback_clock.hpp"
#include "execution/video_scheduler_worker.hpp"
#include "video/video_presenter.hpp"

#include <atomic>
#include <condition_variable>
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
    std::uint64_t recovery_attempts{};
    std::uint64_t recovery_bind_successes{};
    std::uint64_t recovered_presents{};
};

class VideoRenderWorker final {
public:
    VideoRenderWorker(
        PlaybackClock& clock,
        std::unique_ptr<IVideoPresenter> presenter,
        std::shared_ptr<DiagnosticEventJournal> journal = nullptr);
    ~VideoRenderWorker();

    VideoRenderWorker(const VideoRenderWorker&) = delete;
    VideoRenderWorker& operator=(const VideoRenderWorker&) = delete;

    void start(std::shared_ptr<VideoPresentationStream> input);
    void stop();

    [[nodiscard]] VideoOutputSurfaceControlStatus set_output_surface(
        VideoOutputSurfaceDescriptor surface);

    [[nodiscard]] VideoRenderWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] VideoRenderStats stats() const noexcept;

    // The presenter owns thread-safe cumulative DXGI counters and the last
    // HRESULT; reading never calls graphics methods off the render thread.
    [[nodiscard]] VideoPresenterDiagnostics presenter_diagnostics() const noexcept {
        return presenter_->diagnostics();
    }

    [[nodiscard]] VideoPresenterBeginStatus last_begin_status() const noexcept {
        return last_begin_status_.load(std::memory_order_acquire);
    }

    [[nodiscard]] VideoPresentStatus last_present_status() const noexcept {
        return last_present_status_.load(std::memory_order_acquire);
    }

    [[nodiscard]] VideoOutputSurfaceApplyStatus
    last_output_surface_apply_status() const noexcept {
        return last_surface_apply_status_.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] bool apply_requested_surface(
        std::stop_token stop_token,
        std::uint64_t& applied_revision,
        VideoOutputSurfaceDescriptor& applied_surface,
        VideoRenderWorkerState& final_state);
    [[nodiscard]] std::stop_token current_wait_token();
    void interrupt_waits();
    void publish_terminal_state(VideoRenderWorkerState state) noexcept;
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<VideoPresentationStream>& input) noexcept;

    PlaybackClock& clock_;
    std::unique_ptr<IVideoPresenter> presenter_;
    std::shared_ptr<DiagnosticEventJournal> journal_;
    std::atomic<VideoRenderWorkerState> state_{VideoRenderWorkerState::Idle};
    std::atomic<VideoPresenterBeginStatus> last_begin_status_{
        VideoPresenterBeginStatus::Ready
    };
    std::atomic<VideoPresentStatus> last_present_status_{
        VideoPresentStatus::Presented
    };
    std::atomic<VideoOutputSurfaceApplyStatus> last_surface_apply_status_{
        VideoOutputSurfaceApplyStatus::Ready
    };
    std::atomic<std::uint64_t> presented_frames_{};
    std::atomic<std::uint64_t> stale_epoch_drops_{};
    std::atomic<std::uint64_t> inactive_clock_drops_{};
    std::atomic<std::uint64_t> presenter_failures_{};
    std::atomic<std::uint64_t> recovery_attempts_{};
    std::atomic<std::uint64_t> recovery_bind_successes_{};
    std::atomic<std::uint64_t> recovered_presents_{};

    mutable std::mutex lifecycle_mutex_;
    std::mutex surface_command_mutex_;
    std::mutex surface_mutex_;
    std::condition_variable_any surface_changed_;
    VideoOutputSurfaceDescriptor requested_surface_{};
    std::uint64_t requested_surface_revision_{};
    std::uint64_t acknowledged_surface_revision_{};
    VideoOutputSurfaceControlStatus acknowledged_surface_status_{
        VideoOutputSurfaceControlStatus::Deferred
    };
    std::shared_ptr<std::stop_source> wait_interrupt_source_;
    std::jthread worker_;
};

} // namespace lapis::media
