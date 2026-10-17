#pragma once

#include "audio/audio_output.hpp"
#include "core/playback_clock.hpp"

#include <cstdint>

namespace lapis::media {

struct AudioClockDisciplineConfig final {
    // Corrections below this magnitude are left to normal scheduler tolerance.
    MediaTime rebase_threshold{30'000'000};
    // A cached device observation older than this is not trustworthy enough to
    // steer the playback timeline.
    MediaTime maximum_observation_age{200'000'000};
    // QPC/steady-clock correlation can straddle a sampling boundary. Small
    // future timestamps are tolerated without extrapolating backwards.
    MediaTime future_observation_tolerance{2'000'000};
};

enum class AudioClockDisciplineAction : std::uint8_t {
    Ignore,
    Maintain,
    Rebase
};

enum class AudioClockDisciplineReason : std::uint8_t {
    None,
    PlaybackNotRunning,
    HardwareClockUnavailable,
    InvalidMediaPosition,
    StaleObservation,
    FutureObservation,
    WithinThreshold,
    DriftExceeded
};

struct AudioClockDisciplineDecision final {
    AudioClockDisciplineAction action{AudioClockDisciplineAction::Ignore};
    AudioClockDisciplineReason reason{AudioClockDisciplineReason::None};
    // Hardware position extrapolated from observation.sampled_at to `now`.
    MediaTime audio_position_now{};
    // Positive means the hardware audio timeline is ahead of PlaybackClock.
    MediaTime drift{};
    // Propagated so the eventual owner can reject/apply decisions against the
    // output discontinuity generation that produced them.
    std::uint64_t output_revision{};
};

class AudioClockDiscipline final {
public:
    explicit AudioClockDiscipline(AudioClockDisciplineConfig config = {});

    // `playback` must be the snapshot returned by PlaybackClock::snapshot(now)
    // for this same `now`. Keeping both observations on one comparison instant
    // prevents caller scheduling delay from being mistaken for A/V drift.
    [[nodiscard]] AudioClockDisciplineDecision evaluate(
        const AudioOutputPosition& observation,
        const PlaybackClockSnapshot& playback,
        MonotonicTime now) const noexcept;

private:
    AudioClockDisciplineConfig config_;
};

} // namespace lapis::media
