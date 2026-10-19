#include "execution/video_render_worker.hpp"

#include "core/monotonic_time.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lapis::media {
namespace {

// Diagnostic events are emitted only at GPU-recovery transitions, never for
// every frame. Windows DebugView / Visual Studio can capture them without a
// filesystem dependency or callbacks crossing the stable C ABI.
void trace_recovery(
    const char* event,
    std::uint64_t epoch,
    std::uint32_t attempt) noexcept {
#if defined(_WIN32)
    char line[192]{};
    (void)std::snprintf(
        line, sizeof(line),
        "Lapis.VideoRender | event=%s | epoch=%llu | attempt=%u\n",
        event,
        static_cast<unsigned long long>(epoch),
        static_cast<unsigned>(attempt));
    OutputDebugStringA(line);
#else
    (void)event;
    (void)epoch;
    (void)attempt;
#endif
}


VideoOutputSurfaceControlStatus control_status_from_apply_status(
    VideoOutputSurfaceApplyStatus status) noexcept {
    switch (status) {
        case VideoOutputSurfaceApplyStatus::Ready:
            return VideoOutputSurfaceControlStatus::Applied;
        case VideoOutputSurfaceApplyStatus::Unsupported:
            return VideoOutputSurfaceControlStatus::Unsupported;
        case VideoOutputSurfaceApplyStatus::Unavailable:
            return VideoOutputSurfaceControlStatus::Unavailable;
        case VideoOutputSurfaceApplyStatus::Failed:
        default:
            return VideoOutputSurfaceControlStatus::Failed;
    }
}

VideoRenderWorkerState worker_state_from_apply_status(
    VideoOutputSurfaceApplyStatus status) noexcept {
    return status == VideoOutputSurfaceApplyStatus::Unsupported ||
            status == VideoOutputSurfaceApplyStatus::Unavailable
        ? VideoRenderWorkerState::PresenterUnavailable
        : VideoRenderWorkerState::Faulted;
}

} // namespace

VideoRenderWorker::VideoRenderWorker(
    PlaybackClock& clock,
    std::unique_ptr<IVideoPresenter> presenter,
    std::shared_ptr<DiagnosticEventJournal> journal)
    : clock_(clock),
      presenter_(std::move(presenter)),
      journal_(journal != nullptr ? std::move(journal)
                                  : std::make_shared<DiagnosticEventJournal>()),
      wait_interrupt_source_(std::make_shared<std::stop_source>()) {
    if (presenter_ == nullptr) {
        throw std::invalid_argument(
            "VideoRenderWorker requires a presenter.");
    }
    presenter_->set_diagnostics_journal(journal_);
}

VideoRenderWorker::~VideoRenderWorker() {
    stop();
}

void VideoRenderWorker::start(
    std::shared_ptr<VideoPresentationStream> input) {
    if (input == nullptr) {
        throw std::invalid_argument(
            "VideoRenderWorker requires an input stream.");
    }

    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (state_.load(std::memory_order_acquire) ==
        VideoRenderWorkerState::Running) {
        throw std::logic_error("VideoRenderWorker is already running.");
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    presented_frames_.store(0, std::memory_order_relaxed);
    stale_epoch_drops_.store(0, std::memory_order_relaxed);
    inactive_clock_drops_.store(0, std::memory_order_relaxed);
    presenter_failures_.store(0, std::memory_order_relaxed);
    recovery_attempts_.store(0, std::memory_order_relaxed);
    recovery_bind_successes_.store(0, std::memory_order_relaxed);
    recovered_presents_.store(0, std::memory_order_relaxed);
    last_begin_status_.store(
        VideoPresenterBeginStatus::Ready,
        std::memory_order_release);
    last_present_status_.store(
        VideoPresentStatus::Presented,
        std::memory_order_release);
    last_surface_apply_status_.store(
        VideoOutputSurfaceApplyStatus::Ready,
        std::memory_order_release);

    {
        std::scoped_lock surface_lock(surface_mutex_);
        // A desired binding persists across playback generations, but every new
        // render thread must apply it again because end-of-generation teardown
        // releases all presenter references to the native output surface.
        acknowledged_surface_revision_ = 0;
        acknowledged_surface_status_ = VideoOutputSurfaceControlStatus::Deferred;
        wait_interrupt_source_ = std::make_shared<std::stop_source>();
    }

    state_.store(VideoRenderWorkerState::Running, std::memory_order_release);

    try {
        worker_ = std::jthread(
            [this, input = std::move(input)](std::stop_token stop_token) {
                run(stop_token, input);
            });
    } catch (...) {
        publish_terminal_state(VideoRenderWorkerState::Faulted);
        throw;
    }
}

void VideoRenderWorker::stop() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    interrupt_waits();
    surface_changed_.notify_all();
    worker_.join();
}

VideoOutputSurfaceControlStatus VideoRenderWorker::set_output_surface(
    VideoOutputSurfaceDescriptor surface) {
    if (!valid_video_output_surface(surface)) {
        return VideoOutputSurfaceControlStatus::Invalid;
    }

    // Serialize public output-surface mutations so every synchronous caller
    // receives an acknowledgement for its own binding instead of a later caller
    // coalescing and overwriting the revision it is waiting on.
    std::unique_lock command_lock(surface_command_mutex_);
    std::unique_lock surface_lock(surface_mutex_);

    const auto worker_state = state_.load(std::memory_order_acquire);
    if (requested_surface_revision_ == 0 && requested_surface_ == surface) {
        return worker_state == VideoRenderWorkerState::Running
            ? VideoOutputSurfaceControlStatus::Applied
            : VideoOutputSurfaceControlStatus::Deferred;
    }

    std::uint64_t revision = requested_surface_revision_;
    if (!(requested_surface_ == surface)) {
        requested_surface_ = surface;
        revision = ++requested_surface_revision_;
    } else if (revision == 0) {
        revision = ++requested_surface_revision_;
    }

    if (worker_state != VideoRenderWorkerState::Running) {
        acknowledged_surface_revision_ = revision;
        acknowledged_surface_status_ = VideoOutputSurfaceControlStatus::Deferred;
        return VideoOutputSurfaceControlStatus::Deferred;
    }

    if (acknowledged_surface_revision_ >= revision &&
        acknowledged_surface_status_ == VideoOutputSurfaceControlStatus::Applied) {
        return VideoOutputSurfaceControlStatus::Applied;
    }

    if (wait_interrupt_source_ != nullptr) {
        wait_interrupt_source_->request_stop();
    }
    surface_changed_.notify_all();

    surface_changed_.wait(surface_lock, [this, revision] {
        return acknowledged_surface_revision_ >= revision ||
            state_.load(std::memory_order_acquire) !=
                VideoRenderWorkerState::Running;
    });

    if (acknowledged_surface_revision_ >= revision) {
        return acknowledged_surface_status_;
    }

    // The generation ended before it could apply this binding. Presenter teardown
    // has already released the old surface; the desired binding remains stored
    // and will be applied by the next generation.
    return VideoOutputSurfaceControlStatus::Deferred;
}

VideoRenderStats VideoRenderWorker::stats() const noexcept {
    return {
        presented_frames_.load(std::memory_order_relaxed),
        stale_epoch_drops_.load(std::memory_order_relaxed),
        inactive_clock_drops_.load(std::memory_order_relaxed),
        presenter_failures_.load(std::memory_order_relaxed),
        recovery_attempts_.load(std::memory_order_relaxed),
        recovery_bind_successes_.load(std::memory_order_relaxed),
        recovered_presents_.load(std::memory_order_relaxed)
    };
}

bool VideoRenderWorker::apply_requested_surface(
    std::stop_token stop_token,
    std::uint64_t& applied_revision,
    VideoOutputSurfaceDescriptor& applied_surface,
    VideoRenderWorkerState& final_state) {
    for (;;) {
        if (stop_token.stop_requested()) {
            final_state = VideoRenderWorkerState::Stopped;
            return false;
        }

        VideoOutputSurfaceDescriptor requested_surface;
        std::uint64_t requested_revision{};
        {
            std::scoped_lock surface_lock(surface_mutex_);
            requested_surface = requested_surface_;
            requested_revision = requested_surface_revision_;
        }

        if (requested_revision == applied_revision) {
            return true;
        }

        VideoOutputSurfaceApplyStatus apply_status{
            VideoOutputSurfaceApplyStatus::Failed
        };
        try {
            apply_status = presenter_->set_output_surface(requested_surface);
        } catch (...) {
            apply_status = VideoOutputSurfaceApplyStatus::Failed;
        }
        last_surface_apply_status_.store(
            apply_status,
            std::memory_order_release);
        const auto control_status = control_status_from_apply_status(apply_status);

        if (apply_status != VideoOutputSurfaceApplyStatus::Ready) {
            presenter_failures_.fetch_add(1, std::memory_order_relaxed);
            final_state = worker_state_from_apply_status(apply_status);
            {
                std::scoped_lock surface_lock(surface_mutex_);
                acknowledged_surface_revision_ = requested_revision;
                acknowledged_surface_status_ = control_status;
            }
            surface_changed_.notify_all();
            return false;
        }

        applied_revision = requested_revision;
        applied_surface = requested_surface;
        {
            std::scoped_lock surface_lock(surface_mutex_);
            acknowledged_surface_revision_ = requested_revision;
            acknowledged_surface_status_ = VideoOutputSurfaceControlStatus::Applied;

            // Only the render thread renews the wait token, and only after it has
            // observed/applied the revision that caused the interruption. A
            // request racing between this point and pop() therefore leaves a
            // stopped token in place and cannot lose the wake-up.
            if (requested_surface_revision_ == applied_revision &&
                wait_interrupt_source_ != nullptr &&
                wait_interrupt_source_->stop_requested()) {
                wait_interrupt_source_ = std::make_shared<std::stop_source>();
            }
        }
        surface_changed_.notify_all();
    }
}

std::stop_token VideoRenderWorker::current_wait_token() {
    std::scoped_lock surface_lock(surface_mutex_);
    if (wait_interrupt_source_ == nullptr) {
        wait_interrupt_source_ = std::make_shared<std::stop_source>();
    }

    // Do not replace a stopped source here. An output-surface request can race
    // after the render thread's command check and before this call; returning the
    // stopped token makes the bounded queue wait observe that request immediately.
    return wait_interrupt_source_->get_token();
}

void VideoRenderWorker::interrupt_waits() {
    std::scoped_lock surface_lock(surface_mutex_);
    if (wait_interrupt_source_ != nullptr) {
        wait_interrupt_source_->request_stop();
    }
}

void VideoRenderWorker::publish_terminal_state(
    VideoRenderWorkerState state) noexcept {
    state_.store(state, std::memory_order_release);
    {
        std::scoped_lock surface_lock(surface_mutex_);
        if (acknowledged_surface_revision_ < requested_surface_revision_) {
            acknowledged_surface_revision_ = requested_surface_revision_;
            acknowledged_surface_status_ = VideoOutputSurfaceControlStatus::Deferred;
        }
    }
    surface_changed_.notify_all();
}

void VideoRenderWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<VideoPresentationStream>& input) noexcept {
    auto final_state = VideoRenderWorkerState::Stopped;
    bool presenter_started = false;
    std::uint64_t applied_surface_revision{};
    VideoOutputSurfaceDescriptor applied_surface{};
    // Cap recovery work to prevent an endlessly resetting GPU or stale HWND
    // from thrashing the video worker. Every recovery runs on the render thread.
    std::uint32_t device_rebuild_attempts = 0;
    constexpr std::uint32_t max_device_rebuild_attempts = 2;
    // Recreating the device is only a *candidate* recovery. Require a
    // successful Present before treating an immediate EOF as clean.
    bool recovery_pending = false;

    try {
        while (!stop_token.stop_requested()) {
            if (!apply_requested_surface(
                    stop_token,
                    applied_surface_revision,
                    applied_surface,
                    final_state)) {
                break;
            }

            auto read_result = input->pop(current_wait_token());
            if (read_result.status == QueueReadStatus::Stopped) {
                if (stop_token.stop_requested()) {
                    break;
                }
                continue;
            }
            if (read_result.status == QueueReadStatus::Closed) {
                if (recovery_pending) {
                    journal_->record(input->epoch().value(),
                        DiagnosticEventKind::RecoveryIncompleteAtEof, 0, 0, 0,
                        device_rebuild_attempts);
                    trace_recovery("recovered_device_no_present_before_eof",
                        input->epoch().value(), device_rebuild_attempts);
                }
                final_state = recovery_pending
                    ? VideoRenderWorkerState::PresenterUnavailable
                    : VideoRenderWorkerState::Drained;
                break;
            }
            if (!read_result.has_item()) {
                continue;
            }

            const auto& scheduled = *read_result.item;
            const auto clock_snapshot = clock_.snapshot(
                SteadyClockSource::now());

            if (scheduled.frame.epoch.value() != input->epoch().value() ||
                clock_snapshot.epoch.value() != scheduled.frame.epoch.value()) {
                stale_epoch_drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (clock_snapshot.state != PlaybackClockState::Running) {
                inactive_clock_drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (!presenter_started) {
                const auto begin_status = presenter_->begin(input->epoch());
                last_begin_status_.store(begin_status, std::memory_order_release);
                if (begin_status != VideoPresenterBeginStatus::Ready) {
                    presenter_failures_.fetch_add(1, std::memory_order_relaxed);
                    final_state = begin_status == VideoPresenterBeginStatus::Unavailable
                        ? VideoRenderWorkerState::PresenterUnavailable
                        : VideoRenderWorkerState::Faulted;
                    break;
                }
                presenter_started = true;
            }

            const VideoPresentContext context{
                scheduled.frame.epoch,
                scheduled.frame.timestamp,
                scheduled.released_clock_position,
                scheduled.frame_offset,
                clock_snapshot.position
            };

            const auto present_status = presenter_->present(
                scheduled.frame.payload,
                context);
            last_present_status_.store(
                present_status,
                std::memory_order_release);

            if (present_status == VideoPresentStatus::Presented) {
                if (recovery_pending) {
                    recovered_presents_.fetch_add(1, std::memory_order_relaxed);
                    journal_->record(input->epoch().value(),
                        DiagnosticEventKind::RecoveryPresented, 0, 0, 0,
                        device_rebuild_attempts);
                    trace_recovery("recovered_present", input->epoch().value(),
                        device_rebuild_attempts);
                }
                recovery_pending = false;
                presented_frames_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            presenter_failures_.fetch_add(1, std::memory_order_relaxed);

            if (present_status == VideoPresentStatus::DeviceLost &&
                applied_surface.kind != VideoOutputSurfaceKind::None &&
                device_rebuild_attempts < max_device_rebuild_attempts &&
                !stop_token.stop_requested()) {
                ++device_rebuild_attempts;
                recovery_attempts_.fetch_add(1, std::memory_order_relaxed);
                journal_->record(input->epoch().value(),
                    DiagnosticEventKind::RecoveryAttempted, 0, 0, 0,
                    device_rebuild_attempts);
                trace_recovery("device_lost_rebind_start", input->epoch().value(),
                    device_rebuild_attempts);
                // end() drops old D3D/DXGI resources. Rebind from the same
                // desired descriptor; do not touch the clock/epoch or ask the
                // UI thread to create a device. The failed frame is discarded.
                presenter_->end();
                presenter_started = false;
                VideoOutputSurfaceApplyStatus recovery{
                    VideoOutputSurfaceApplyStatus::Failed
                };
                try {
                    recovery = presenter_->set_output_surface(applied_surface);
                } catch (...) {
                    recovery = VideoOutputSurfaceApplyStatus::Failed;
                }
                last_surface_apply_status_.store(
                    recovery, std::memory_order_release);
                if (recovery == VideoOutputSurfaceApplyStatus::Ready) {
                    recovery_bind_successes_.fetch_add(
                        1, std::memory_order_relaxed);
                    journal_->record(input->epoch().value(),
                        DiagnosticEventKind::RecoveryBindSucceeded, 0, 0, 0,
                        device_rebuild_attempts);
                    trace_recovery("device_lost_rebind_ready",
                        input->epoch().value(), device_rebuild_attempts);
                    recovery_pending = true;
                    continue;
                }
                journal_->record(input->epoch().value(),
                    DiagnosticEventKind::RecoveryBindFailed,
                    static_cast<std::uint32_t>(recovery), 0, 0,
                    device_rebuild_attempts);
                trace_recovery("device_lost_rebind_failed",
                    input->epoch().value(), device_rebuild_attempts);
                final_state = worker_state_from_apply_status(recovery);
                break;
            }

            if (present_status == VideoPresentStatus::DeviceLost) {
                journal_->record(input->epoch().value(),
                    DiagnosticEventKind::RecoveryTerminated, 0, 0, 0,
                    device_rebuild_attempts);
                trace_recovery("device_lost_terminal",
                    input->epoch().value(), device_rebuild_attempts);
            }
            final_state =
                present_status == VideoPresentStatus::UnsupportedSurface ||
                present_status == VideoPresentStatus::DeviceLost
                    ? VideoRenderWorkerState::PresenterUnavailable
                    : VideoRenderWorkerState::Faulted;
            break;
        }
    } catch (...) {
        presenter_failures_.fetch_add(1, std::memory_order_relaxed);
        final_state = VideoRenderWorkerState::Faulted;
    }

    if (presenter_started) {
        presenter_->end();
    }

    // Presenters must not retain native-window references while the generation is
    // inactive. The desired descriptor remains in the worker and is re-applied on
    // the next start; this explicit detach makes stopped/deferred rebinding safe.
    if (applied_surface.kind != VideoOutputSurfaceKind::None) {
        VideoOutputSurfaceApplyStatus detach_status{
            VideoOutputSurfaceApplyStatus::Failed
        };
        try {
            detach_status = presenter_->set_output_surface(
                VideoOutputSurfaceDescriptor{});
        } catch (...) {
            detach_status = VideoOutputSurfaceApplyStatus::Failed;
        }

        if (detach_status != VideoOutputSurfaceApplyStatus::Ready) {
            last_surface_apply_status_.store(
                detach_status,
                std::memory_order_release);
            presenter_failures_.fetch_add(1, std::memory_order_relaxed);
            final_state = worker_state_from_apply_status(detach_status);
        }
    }

    publish_terminal_state(final_state);
}

} // namespace lapis::media
