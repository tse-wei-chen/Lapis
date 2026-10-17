#pragma once

#include "audio/audio_output.hpp"
#include "core/playback_clock.hpp"
#include "execution/audio_clock_discipline.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

struct AudioClockSyncConfig final {
    AudioClockDisciplineConfig discipline{};
    MediaTime sample_interval{20'000'000};
};

enum class AudioClockSyncWorkerState : std::uint8_t {
    Idle,
    Running,
    Stopped,
    Faulted
};

struct AudioClockSyncStats final {
    std::uint64_t evaluations{};
    std::uint64_t ignored{};
    std::uint64_t maintained{};
    std::uint64_t rebase_recommendations{};
    std::uint64_t rebases_applied{};
    std::uint64_t rebases_rejected{};
    AudioClockDisciplineAction last_action{AudioClockDisciplineAction::Ignore};
    AudioClockDisciplineReason last_reason{AudioClockDisciplineReason::None};
    MediaTime last_audio_position{};
    MediaTime last_drift{};
    std::uint64_t last_output_revision{};
};

using AudioOutputPositionReader = std::function<AudioOutputPosition()>;

class AudioClockSyncWorker final {
public:
    AudioClockSyncWorker(
        PlaybackClock& clock,
        AudioOutputPositionReader position_reader,
        AudioClockSyncConfig config = {});
    ~AudioClockSyncWorker();

    AudioClockSyncWorker(const AudioClockSyncWorker&) = delete;
    AudioClockSyncWorker& operator=(const AudioClockSyncWorker&) = delete;

    void start();
    void stop();

    [[nodiscard]] AudioClockSyncWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] AudioClockSyncStats stats() const noexcept;

private:
    void run(std::stop_token stop_token) noexcept;
    void record_decision(const AudioClockDisciplineDecision& decision) noexcept;

    PlaybackClock& clock_;
    AudioOutputPositionReader position_reader_;
    AudioClockDiscipline discipline_;
    const MediaTime sample_interval_;

    std::atomic<AudioClockSyncWorkerState> state_{
        AudioClockSyncWorkerState::Idle
    };
    std::atomic<std::uint64_t> evaluations_{};
    std::atomic<std::uint64_t> ignored_{};
    std::atomic<std::uint64_t> maintained_{};
    std::atomic<std::uint64_t> rebase_recommendations_{};
    std::atomic<std::uint64_t> rebases_applied_{};
    std::atomic<std::uint64_t> rebases_rejected_{};
    std::atomic<AudioClockDisciplineAction> last_action_{
        AudioClockDisciplineAction::Ignore
    };
    std::atomic<AudioClockDisciplineReason> last_reason_{
        AudioClockDisciplineReason::None
    };
    std::atomic<std::int64_t> last_audio_position_ns_{};
    std::atomic<std::int64_t> last_drift_ns_{};
    std::atomic<std::uint64_t> last_output_revision_{};

    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
