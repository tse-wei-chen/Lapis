#include "execution/audio_clock_sync_worker.hpp"

#include "core/monotonic_time.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace lapis::media {

AudioClockSyncWorker::AudioClockSyncWorker(
    PlaybackClock& clock,
    AudioOutputPositionReader position_reader,
    AudioClockSyncConfig config)
    : clock_(clock),
      position_reader_(std::move(position_reader)),
      discipline_(config.discipline),
      sample_interval_(config.sample_interval) {
    if (!position_reader_) {
        throw std::invalid_argument(
            "AudioClockSyncWorker requires a position reader.");
    }
    if (sample_interval_.nanoseconds <= 0) {
        throw std::invalid_argument(
            "AudioClockSyncWorker sample interval must be greater than zero.");
    }
}

AudioClockSyncWorker::~AudioClockSyncWorker() {
    stop();
}

void AudioClockSyncWorker::start() {
    std::scoped_lock lock(control_mutex_);
    if (state_.load(std::memory_order_acquire) ==
        AudioClockSyncWorkerState::Running) {
        throw std::logic_error("AudioClockSyncWorker is already running.");
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    evaluations_.store(0, std::memory_order_relaxed);
    ignored_.store(0, std::memory_order_relaxed);
    maintained_.store(0, std::memory_order_relaxed);
    rebase_recommendations_.store(0, std::memory_order_relaxed);
    rebases_applied_.store(0, std::memory_order_relaxed);
    rebases_rejected_.store(0, std::memory_order_relaxed);
    last_action_.store(
        AudioClockDisciplineAction::Ignore,
        std::memory_order_release);
    last_reason_.store(
        AudioClockDisciplineReason::None,
        std::memory_order_release);
    last_audio_position_ns_.store(0, std::memory_order_relaxed);
    last_drift_ns_.store(0, std::memory_order_relaxed);
    last_output_revision_.store(0, std::memory_order_relaxed);
    state_.store(AudioClockSyncWorkerState::Running, std::memory_order_release);

    try {
        worker_ = std::jthread(
            [this](std::stop_token stop_token) {
                run(stop_token);
            });
    } catch (...) {
        state_.store(AudioClockSyncWorkerState::Faulted, std::memory_order_release);
        throw;
    }
}

void AudioClockSyncWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    worker_.join();
}

AudioClockSyncStats AudioClockSyncWorker::stats() const noexcept {
    return {
        evaluations_.load(std::memory_order_relaxed),
        ignored_.load(std::memory_order_relaxed),
        maintained_.load(std::memory_order_relaxed),
        rebase_recommendations_.load(std::memory_order_relaxed),
        rebases_applied_.load(std::memory_order_relaxed),
        rebases_rejected_.load(std::memory_order_relaxed),
        last_action_.load(std::memory_order_acquire),
        last_reason_.load(std::memory_order_acquire),
        MediaTime{last_audio_position_ns_.load(std::memory_order_relaxed)},
        MediaTime{last_drift_ns_.load(std::memory_order_relaxed)},
        last_output_revision_.load(std::memory_order_relaxed)
    };
}

void AudioClockSyncWorker::record_decision(
    const AudioClockDisciplineDecision& decision) noexcept {
    evaluations_.fetch_add(1, std::memory_order_relaxed);
    last_action_.store(decision.action, std::memory_order_release);
    last_reason_.store(decision.reason, std::memory_order_release);
    last_audio_position_ns_.store(
        decision.audio_position_now.nanoseconds,
        std::memory_order_relaxed);
    last_drift_ns_.store(decision.drift.nanoseconds, std::memory_order_relaxed);
    last_output_revision_.store(
        decision.output_revision,
        std::memory_order_relaxed);

    switch (decision.action) {
        case AudioClockDisciplineAction::Ignore:
            ignored_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AudioClockDisciplineAction::Maintain:
            maintained_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AudioClockDisciplineAction::Rebase:
            rebase_recommendations_.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void AudioClockSyncWorker::run(std::stop_token stop_token) noexcept {
    auto final_state = AudioClockSyncWorkerState::Stopped;

    try {
        while (!stop_token.stop_requested()) {
            const auto now = SteadyClockSource::now();
            const auto playback = clock_.snapshot(now);

            if (playback.state == PlaybackClockState::Running) {
                const auto observation = position_reader_();
                const auto decision = discipline_.evaluate(
                    observation,
                    playback,
                    now);
                record_decision(decision);

                if (decision.action == AudioClockDisciplineAction::Rebase) {
                    // Re-read the source revision immediately before applying a
                    // correction. Reset/seek/device-loss invalidates the cached
                    // hardware observation independently of PlaybackClock.
                    const auto latest = position_reader_();
                    const bool source_still_current =
                        latest.source == AudioOutputPositionSource::HardwareClock &&
                        latest.revision == decision.output_revision;

                    if (source_still_current &&
                        clock_.try_rebase(
                            playback,
                            decision.audio_position_now,
                            now)) {
                        rebases_applied_.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        rebases_rejected_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            if (stop_token.stop_requested()) {
                break;
            }

            const auto wait_snapshot = clock_.snapshot(SteadyClockSource::now());
            PlaybackClockWaitStatus wait_status{};
            if (wait_snapshot.state == PlaybackClockState::Running) {
                wait_status = clock_.wait_for_change_for(
                    wait_snapshot.revision,
                    std::chrono::nanoseconds{sample_interval_.nanoseconds},
                    stop_token);
            } else {
                // There is no reason to poll a hardware clock while logical
                // playback is paused/stopped. A clock transition wakes us.
                wait_status = clock_.wait_for_change(
                    wait_snapshot.revision,
                    stop_token);
            }

            if (wait_status == PlaybackClockWaitStatus::Stopped) {
                break;
            }
        }
    } catch (...) {
        final_state = AudioClockSyncWorkerState::Faulted;
    }

    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
