#include "execution/playback_execution.hpp"

#include "core/monotonic_time.hpp"
#include "session/playback_media_session.hpp"

#include <utility>

namespace lapis::media {

PlaybackExecution::PlaybackExecution(
    PlaybackMediaSession& media,
    std::unique_ptr<IVideoPresenter> presenter,
    PlaybackExecutionConfig config)
    : media_(media),
      decoder_(media_, config.decode_queue_capacity),
      scheduler_(
          clock_,
          config.scheduler,
          config.presentation_queue_capacity),
      renderer_(clock_, std::move(presenter)) {}

PlaybackExecution::~PlaybackExecution() {
    try {
        std::scoped_lock lock(control_mutex_);
        const auto now = SteadyClockSource::now();
        const auto clock_snapshot = clock_.snapshot(now);
        if (clock_snapshot.state != PlaybackClockState::Stopped) {
            (void)clock_.stop(now);
        }
        stop_generation_unlocked();
    } catch (...) {
        // Destructors must not surface thread-shutdown failures. Public ABI
        // owners perform explicit close before destruction and can report the
        // corresponding lifecycle error there.
    }
}

PlaybackExecutionResult PlaybackExecution::start(
    PlaybackEpoch epoch,
    MediaTime position) {
    if (position.nanoseconds < 0) {
        return {PlaybackExecutionError::InvalidArgument};
    }

    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Stopped) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (!epoch_advances_unlocked(epoch)) {
        return {PlaybackExecutionError::StaleEpoch};
    }

    const auto rebase_time = SteadyClockSource::now();
    if (!clock_.rebase(epoch, position, rebase_time)) {
        return {PlaybackExecutionError::ClockFailure};
    }

    if (!start_generation_unlocked(epoch)) {
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerStartFailed};
    }

    if (!clock_.start(position, SteadyClockSource::now())) {
        stop_generation_unlocked();
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    logical_state_ = PlaybackExecutionState::Running;
    return {};
}

PlaybackExecutionResult PlaybackExecution::pause() {
    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Running) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (workers_failed_unlocked()) {
        stop_generation_unlocked();
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerFailed};
    }

    if (!clock_.pause(SteadyClockSource::now())) {
        stop_generation_unlocked();
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    logical_state_ = PlaybackExecutionState::Paused;
    return {};
}

PlaybackExecutionResult PlaybackExecution::resume() {
    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Paused) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (workers_failed_unlocked()) {
        stop_generation_unlocked();
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerFailed};
    }

    if (!clock_.resume(SteadyClockSource::now())) {
        stop_generation_unlocked();
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    logical_state_ = PlaybackExecutionState::Running;
    return {};
}

PlaybackExecutionResult PlaybackExecution::seek(
    PlaybackEpoch epoch,
    MediaTime position) {
    if (position.nanoseconds < 0) {
        return {PlaybackExecutionError::InvalidArgument};
    }

    std::scoped_lock lock(control_mutex_);
    if (logical_state_ != PlaybackExecutionState::Running &&
        logical_state_ != PlaybackExecutionState::Paused &&
        logical_state_ != PlaybackExecutionState::Stopped) {
        return {PlaybackExecutionError::InvalidState};
    }
    if (!epoch_advances_unlocked(epoch)) {
        return {PlaybackExecutionError::StaleEpoch};
    }
    if (workers_failed_unlocked()) {
        stop_generation_unlocked();
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerFailed};
    }

    const auto prior_state = logical_state_;
    if (prior_state == PlaybackExecutionState::Running &&
        !clock_.pause(SteadyClockSource::now())) {
        stop_generation_unlocked();
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    // Stop downstream first. Each upstream producer may be blocked on the
    // bounded queue owned by the stage below it.
    stop_generation_unlocked();

    if (!media_.seek(position)) {
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::MediaSeekFailed};
    }

    if (!clock_.rebase(epoch, position, SteadyClockSource::now())) {
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    if (prior_state != PlaybackExecutionState::Stopped &&
        !start_generation_unlocked(epoch)) {
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::WorkerStartFailed};
    }

    if (prior_state == PlaybackExecutionState::Running &&
        !clock_.resume(SteadyClockSource::now())) {
        stop_generation_unlocked();
        clock_.reset(epoch, SteadyClockSource::now());
        logical_state_ = PlaybackExecutionState::Faulted;
        return {PlaybackExecutionError::ClockFailure};
    }

    logical_state_ = prior_state;
    return {};
}

PlaybackExecutionResult PlaybackExecution::close(PlaybackEpoch epoch) {
    std::scoped_lock lock(control_mutex_);
    if (!epoch_advances_unlocked(epoch)) {
        return {PlaybackExecutionError::StaleEpoch};
    }

    const auto now = SteadyClockSource::now();
    const auto clock_snapshot = clock_.snapshot(now);
    if (clock_snapshot.state != PlaybackClockState::Stopped) {
        (void)clock_.stop(now);
    }

    stop_generation_unlocked();
    clock_.reset(epoch, SteadyClockSource::now());
    logical_state_ = PlaybackExecutionState::Stopped;
    return {};
}

PlaybackExecutionSnapshot PlaybackExecution::snapshot() const {
    std::scoped_lock lock(control_mutex_);

    auto state = logical_state_;
    if (workers_failed_unlocked()) {
        state = PlaybackExecutionState::Faulted;
    } else if ((state == PlaybackExecutionState::Running ||
                state == PlaybackExecutionState::Paused) &&
               workers_drained_unlocked()) {
        state = PlaybackExecutionState::Ended;
    }

    return {
        state,
        clock_.snapshot(SteadyClockSource::now()),
        decoder_.state(),
        scheduler_.state(),
        renderer_.state(),
        scheduler_.stats(),
        renderer_.stats()
    };
}

bool PlaybackExecution::epoch_advances_unlocked(PlaybackEpoch epoch) const {
    return epoch.value() >
        clock_.snapshot(SteadyClockSource::now()).epoch.value();
}

bool PlaybackExecution::workers_failed_unlocked() const noexcept {
    const auto decode_state = decoder_.state();
    const auto scheduler_state = scheduler_.state();
    const auto render_state = renderer_.state();

    return decode_state == VideoDecodeWorkerState::Faulted ||
        scheduler_state == VideoSchedulerWorkerState::Faulted ||
        render_state == VideoRenderWorkerState::PresenterUnavailable ||
        render_state == VideoRenderWorkerState::Faulted;
}

bool PlaybackExecution::workers_drained_unlocked() const noexcept {
    return decoder_.state() == VideoDecodeWorkerState::Drained &&
        scheduler_.state() == VideoSchedulerWorkerState::Drained &&
        renderer_.state() == VideoRenderWorkerState::Drained;
}

bool PlaybackExecution::start_generation_unlocked(PlaybackEpoch epoch) {
    try {
        auto decoded = decoder_.start(epoch);
        auto presentation = scheduler_.start(std::move(decoded));
        renderer_.start(std::move(presentation));
        return true;
    } catch (...) {
        try {
            stop_generation_unlocked();
        } catch (...) {
            // The caller will transition the whole execution to Faulted.
        }
        return false;
    }
}

void PlaybackExecution::stop_generation_unlocked() {
    renderer_.stop();
    scheduler_.stop();
    decoder_.stop();
}

} // namespace lapis::media
