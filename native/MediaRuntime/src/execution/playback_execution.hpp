#pragma once

#include "audio/audio_output_provider.hpp"
#include "audio/audio_processor.hpp"
#include "core/graph_plan.hpp"
#include "core/media_time.hpp"
#include "core/playback_clock.hpp"
#include "core/playback_epoch.hpp"
#include "execution/audio_clock_sync_worker.hpp"
#include "execution/audio_decode_worker.hpp"
#include "execution/audio_process_worker.hpp"
#include "execution/audio_render_worker.hpp"
#include "execution/video_decode_worker.hpp"
#include "execution/video_process_worker.hpp"
#include "execution/video_render_worker.hpp"
#include "execution/video_scheduler_worker.hpp"
#include "video/video_presenter.hpp"
#include "video/video_processor.hpp"
#include "video/video_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace lapis::media {

class PlaybackMediaSession;

using AudioFrameProcessorFactory =
    std::function<std::unique_ptr<IAudioFrameProcessor>(const AudioFormat&)>;

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
    std::size_t process_queue_capacity{4};
    std::size_t presentation_queue_capacity{2};
    VideoSchedulerConfig scheduler{};
    std::size_t audio_decode_queue_capacity{16};
    std::size_t audio_process_queue_capacity{8};
    AudioClockSyncConfig audio_clock_sync{};
};

struct PlaybackExecutionSnapshot final {
    PlaybackExecutionState state{PlaybackExecutionState::Stopped};
    PlaybackClockSnapshot clock{};
    bool video_active{};
    bool audio_active{};
    VideoDecodeWorkerState decode_state{VideoDecodeWorkerState::Idle};
    VideoProcessWorkerState process_state{VideoProcessWorkerState::Idle};
    VideoProcessStatus process_status{VideoProcessStatus::Processed};
    VideoSchedulerWorkerState scheduler_state{VideoSchedulerWorkerState::Idle};
    VideoRenderWorkerState render_state{VideoRenderWorkerState::Idle};
    VideoSchedulingStats scheduling{};
    VideoRenderStats rendering{};
    VideoPresenterDiagnostics presenter_diagnostics{};
    AudioDecodeWorkerState audio_decode_state{AudioDecodeWorkerState::Idle};
    AudioProcessWorkerState audio_process_state{AudioProcessWorkerState::Idle};
    AudioProcessStatus audio_process_status{AudioProcessStatus::NeedMoreInput};
    AudioRenderWorkerState audio_render_state{AudioRenderWorkerState::Idle};
    AudioRenderStats audio_rendering{};
    AudioOutputPosition audio_position{};
    AudioClockSyncWorkerState audio_clock_sync_state{
        AudioClockSyncWorkerState::Idle
    };
    AudioClockSyncStats audio_clock_sync{};
};

class PlaybackExecution final {
public:
    PlaybackExecution(
        PlaybackMediaSession& media,
        std::unique_ptr<IVideoFrameProcessor> processor,
        std::unique_ptr<IVideoPresenter> presenter,
        PlaybackExecutionConfig config = {},
        std::shared_ptr<DiagnosticEventJournal> journal = nullptr);

    PlaybackExecution(
        PlaybackMediaSession& media,
        std::unique_ptr<IVideoFrameProcessor> processor,
        std::unique_ptr<IVideoPresenter> presenter,
        std::unique_ptr<IAudioOutputProvider> audio_output_provider,
        AudioFrameProcessorFactory audio_processor_factory,
        PlaybackExecutionConfig config = {},
        std::shared_ptr<DiagnosticEventJournal> journal = nullptr);

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

    // Output-surface rebinding is presentation control, not a playback
    // discontinuity. Serialize it with transport lifecycle so a public adapter
    // never races generation start/stop while the render worker acknowledges it.
    [[nodiscard]] VideoOutputSurfaceControlStatus set_video_output_surface(
        VideoOutputSurfaceDescriptor surface) {
        std::scoped_lock lock(control_mutex_);
        return renderer_.set_output_surface(surface);
    }

    [[nodiscard]] PlaybackExecutionSnapshot snapshot() const;
    // Return a value copy under the control lock: no caller may hold a raw
    // pointer into a retiring graph generation across Seek or Close.
    [[nodiscard]] std::optional<GraphPlan> active_graph_plan() const;

private:
    [[nodiscard]] PlaybackExecutionResult fault_unlocked(
        PlaybackExecutionError error);
    [[nodiscard]] bool epoch_advances_unlocked(PlaybackEpoch epoch) const;
    [[nodiscard]] bool start_epoch_valid_unlocked(
        PlaybackEpoch epoch,
        MediaTime position) const;
    [[nodiscard]] bool workers_failed_unlocked() const noexcept;
    [[nodiscard]] bool workers_drained_unlocked() const noexcept;
    [[nodiscard]] bool start_generation_unlocked(PlaybackEpoch epoch);
    [[nodiscard]] bool pause_audio_unlocked();
    [[nodiscard]] bool resume_audio_unlocked();
    [[nodiscard]] bool start_audio_clock_sync_unlocked();
    void stop_generation_unlocked();

    PlaybackMediaSession& media_;
    mutable PlaybackClock clock_;
    VideoDecodeWorker decoder_;
    VideoProcessWorker processor_;
    VideoSchedulerWorker scheduler_;
    VideoRenderWorker renderer_;
    AudioDecodeWorker audio_decoder_;
    std::unique_ptr<IAudioOutputProvider> audio_output_provider_;
    AudioFrameProcessorFactory audio_processor_factory_;
    const std::size_t audio_process_queue_capacity_;
    const AudioClockSyncConfig audio_clock_sync_config_;
    std::unique_ptr<AudioProcessWorker> audio_processor_;
    std::unique_ptr<AudioRenderWorker> audio_renderer_;
    std::unique_ptr<AudioClockSyncWorker> audio_clock_sync_;
    std::shared_ptr<AudioFrameStream> pending_audio_render_stream_;
    std::optional<GraphPlan> active_graph_plan_;
    bool video_generation_active_{};
    bool audio_generation_active_{};
    bool stopped_timeline_prepared_{};
    mutable std::mutex control_mutex_;
    PlaybackExecutionState logical_state_{PlaybackExecutionState::Stopped};
};

} // namespace lapis::media
