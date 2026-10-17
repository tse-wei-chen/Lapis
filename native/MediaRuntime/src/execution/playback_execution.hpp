#pragma once

#include "core/media_time.hpp"
#include "core/playback_clock.hpp"
#include "core/playback_epoch.hpp"
#include "execution/video_decode_worker.hpp"
#include "execution/video_render_worker.hpp"
#include "execution/video_scheduler_worker.hpp"
#include "video/video_presenter.hpp"
#include "video/video_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace lapis::media {

class PlaybackMediaSession;

enum class PlaybackExecutionState : std::uint8_t {
    Stopped,
    Running,
    Paused,
    Ended,
    Faulted
};

enum class PlaybackExecutionError : std::uint8_t {
    None,
    InvalidArgument,
    InvalidState,
    StaleEpoch,
    MediaSeekFailed,
    ClockFailure,
    WorkerStartFailed,
    WorkerFailed
};

struct PlaybackExecutionResult final {
    PlaybackExecutionError error{PlaybackExecutionError::None};

    [[nodiscard]] bool ok() const noexcept {
        return error == PlaybackExecutionError::None;
    }
};

struct PlaybackExecutionConfig final {
    std::size_t decode_queue_capacity{8};
    std::size_t presentation_queue_capacity{2};
    VideoSchedulerConfig scheduler{};
};

struct PlaybackExecutionSnapshot final {
    PlaybackExecutionState state{PlaybackExecutionState::Stopped};
    PlaybackClockSnapshot clock{};
    VideoDecodeWorkerState decode_state{VideoDecodeWorkerState::Idle};
    VideoSchedulerWorkerState scheduler_state{VideoSchedulerWorkerState::Idle};
    VideoRenderWorkerState render_state{VideoRenderWorkerState::Idle};
    VideoSchedulingStats scheduling{};
    VideoRenderStats rendering{};
};

class PlaybackExecution final {
public:
    PlaybackExecution(
        PlaybackMediaSession& media,
        std::unique_ptr<IVideoPresenter> presenter,
        PlaybackExecutionConfig config = {});
    ~PlaybackExecution();

    PlaybackExecution(const PlaybackExecution&) = delete;
    PlaybackExecution& operator=(const PlaybackExecution&) = delete;

    [[nodiscard]] PlaybackExecutionResult start(
        PlaybackEpoch epoch,
        MediaTime position);
    [[nodiscard]] PlaybackExecutionResult pause();
    [[nodiscard]] PlaybackExecutionResult resume();
    [[nodiscard]] PlaybackExecutionResult seek(
        PlaybackEpoch epoch,
        MediaTime position);
    [[nodiscard]] PlaybackExecutionResult close(PlaybackEpoch epoch);

    [[nodiscard]] PlaybackExecutionSnapshot snapshot() const;

private:
    [[nodiscard]] bool epoch_advances_unlocked(PlaybackEpoch epoch) const;
    [[nodiscard]] bool workers_failed_unlocked() const noexcept;
    [[nodiscard]] bool workers_drained_unlocked() const noexcept;
    [[nodiscard]] bool start_generation_unlocked(PlaybackEpoch epoch);
    void stop_generation_unlocked();

    PlaybackMediaSession& media_;
    mutable PlaybackClock clock_;
    VideoDecodeWorker decoder_;
    VideoSchedulerWorker scheduler_;
    VideoRenderWorker renderer_;
    mutable std::mutex control_mutex_;
    PlaybackExecutionState logical_state_{PlaybackExecutionState::Stopped};
};

} // namespace lapis::media
