#include "execution/audio_clock_discipline.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

lapis::media::PlaybackClockSnapshot running_clock(
    std::int64_t position_ns) {
    using namespace lapis::media;
    return {
        PlaybackClockState::Running,
        PlaybackEpoch{7},
        MediaTime{position_ns},
        11
    };
}

lapis::media::AudioOutputPosition hardware_observation(
    std::int64_t position_ns,
    std::uint64_t sampled_at_ns,
    std::uint64_t revision = 3) {
    using namespace lapis::media;
    return {
        MediaTime{position_ns},
        MonotonicTime{sampled_at_ns},
        revision,
        AudioOutputPositionSource::HardwareClock
    };
}

} // namespace

int main() {
    using namespace lapis::media;

    // Invalid policy values fail at construction rather than producing ambiguous
    // decisions in the timing hot path.
    {
        auto config = AudioClockDisciplineConfig{};
        config.rebase_threshold = MediaTime{-1};
        bool threw{};
        try {
            (void)AudioClockDiscipline(config);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    AudioClockDiscipline discipline;
    constexpr auto now = MonotonicTime{10'000'000'000ULL};

    // Only a hardware-correlated source is allowed to discipline playback.
    {
        auto observation = hardware_observation(4'000'000'000LL, now.nanoseconds);
        observation.source = AudioOutputPositionSource::Estimated;
        const auto decision = discipline.evaluate(
            observation,
            running_clock(4'000'000'000LL),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Ignore);
        CHECK(decision.reason ==
            AudioClockDisciplineReason::HardwareClockUnavailable);
        CHECK(decision.output_revision == observation.revision);
    }

    // Paused/stopped playback never extrapolates the hardware clock into a
    // correction because the playback timeline is intentionally frozen.
    {
        auto playback = running_clock(4'000'000'000LL);
        playback.state = PlaybackClockState::Paused;
        const auto decision = discipline.evaluate(
            hardware_observation(4'000'000'000LL, now.nanoseconds),
            playback,
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Ignore);
        CHECK(decision.reason ==
            AudioClockDisciplineReason::PlaybackNotRunning);
    }

    // Negative media positions are not legal correction anchors for the current
    // PlaybackClock contract.
    {
        const auto decision = discipline.evaluate(
            hardware_observation(-1, now.nanoseconds),
            running_clock(0),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Ignore);
        CHECK(decision.reason ==
            AudioClockDisciplineReason::InvalidMediaPosition);
    }

    // A cached observation is aged to `now` at normal playback rate before drift
    // is computed. Here both clocks resolve to exactly 4.100 s.
    {
        const auto decision = discipline.evaluate(
            hardware_observation(
                4'000'000'000LL,
                now.nanoseconds - 100'000'000ULL),
            running_clock(4'100'000'000LL),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Maintain);
        CHECK(decision.reason == AudioClockDisciplineReason::WithinThreshold);
        CHECK(decision.audio_position_now.nanoseconds == 4'100'000'000LL);
        CHECK(decision.drift.nanoseconds == 0);
    }

    // The default 30 ms deadband suppresses tiny corrections and therefore does
    // not wake PlaybackClock waiters on every hardware observation.
    {
        const auto decision = discipline.evaluate(
            hardware_observation(4'025'000'000LL, now.nanoseconds),
            running_clock(4'000'000'000LL),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Maintain);
        CHECK(decision.drift.nanoseconds == 25'000'000LL);
    }

    // Exactly at the threshold still maintains the existing clock anchor.
    {
        const auto decision = discipline.evaluate(
            hardware_observation(4'030'000'000LL, now.nanoseconds),
            running_clock(4'000'000'000LL),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Maintain);
        CHECK(decision.drift.nanoseconds == 30'000'000LL);
    }

    // Drift beyond the deadband produces a target position but does not mutate
    // PlaybackClock itself; orchestration owns that side effect.
    {
        const auto decision = discipline.evaluate(
            hardware_observation(4'040'000'000LL, now.nanoseconds, 19),
            running_clock(4'000'000'000LL),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Rebase);
        CHECK(decision.reason == AudioClockDisciplineReason::DriftExceeded);
        CHECK(decision.audio_position_now.nanoseconds == 4'040'000'000LL);
        CHECK(decision.drift.nanoseconds == 40'000'000LL);
        CHECK(decision.output_revision == 19);
    }

    {
        const auto decision = discipline.evaluate(
            hardware_observation(3'950'000'000LL, now.nanoseconds),
            running_clock(4'000'000'000LL),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Rebase);
        CHECK(decision.drift.nanoseconds == -50'000'000LL);
    }

    // The default freshness window rejects old cached data rather than applying a
    // large extrapolation from an observation the device has not refreshed.
    {
        const auto decision = discipline.evaluate(
            hardware_observation(
                4'000'000'000LL,
                now.nanoseconds - 200'000'001ULL),
            running_clock(4'200'000'000LL),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Ignore);
        CHECK(decision.reason == AudioClockDisciplineReason::StaleObservation);
    }

    // Small future correlation error is tolerated without backwards
    // extrapolation; a larger future timestamp is rejected.
    {
        const auto tolerated = discipline.evaluate(
            hardware_observation(
                4'000'000'000LL,
                now.nanoseconds + 2'000'000ULL),
            running_clock(4'000'000'000LL),
            now);
        CHECK(tolerated.action == AudioClockDisciplineAction::Maintain);
        CHECK(tolerated.audio_position_now.nanoseconds == 4'000'000'000LL);

        const auto rejected = discipline.evaluate(
            hardware_observation(
                4'000'000'000LL,
                now.nanoseconds + 2'000'001ULL),
            running_clock(4'000'000'000LL),
            now);
        CHECK(rejected.action == AudioClockDisciplineAction::Ignore);
        CHECK(rejected.reason == AudioClockDisciplineReason::FutureObservation);
    }

    // Extrapolation and drift arithmetic saturate instead of overflowing at the
    // representable MediaTime limits.
    {
        const auto near_max = std::numeric_limits<std::int64_t>::max() - 10;
        const auto decision = discipline.evaluate(
            hardware_observation(
                near_max,
                now.nanoseconds - 100ULL),
            running_clock(0),
            now);
        CHECK(decision.action == AudioClockDisciplineAction::Rebase);
        CHECK(decision.audio_position_now.nanoseconds ==
            std::numeric_limits<std::int64_t>::max());
        CHECK(decision.drift.nanoseconds ==
            std::numeric_limits<std::int64_t>::max());
    }

    return 0;
}
