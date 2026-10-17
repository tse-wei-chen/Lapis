#include "execution/audio_clock_discipline.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace lapis::media {
namespace {

std::int64_t saturating_add_nonnegative(
    std::int64_t left,
    std::uint64_t right) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (left < 0) {
        return left;
    }

    const auto unsigned_left = static_cast<std::uint64_t>(left);
    const auto maximum_unsigned = static_cast<std::uint64_t>(maximum);
    if (right > maximum_unsigned - unsigned_left) {
        return maximum;
    }
    return static_cast<std::int64_t>(unsigned_left + right);
}

std::int64_t saturating_subtract(
    std::int64_t left,
    std::int64_t right) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    if (right > 0 && left < minimum + right) {
        return minimum;
    }
    if (right < 0 && left > maximum + right) {
        return maximum;
    }
    return left - right;
}

std::uint64_t nonnegative_duration(MediaTime value) noexcept {
    return value.nanoseconds > 0
        ? static_cast<std::uint64_t>(value.nanoseconds)
        : 0U;
}

std::uint64_t magnitude(std::int64_t value) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) + 1U;
    }
    return static_cast<std::uint64_t>(-value);
}

AudioClockDisciplineDecision ignored(
    AudioClockDisciplineReason reason,
    std::uint64_t output_revision) noexcept {
    return {
        AudioClockDisciplineAction::Ignore,
        reason,
        MediaTime{},
        MediaTime{},
        output_revision
    };
}

} // namespace

AudioClockDiscipline::AudioClockDiscipline(AudioClockDisciplineConfig config)
    : config_(config) {
    if (config_.rebase_threshold.nanoseconds < 0 ||
        config_.maximum_observation_age.nanoseconds < 0 ||
        config_.future_observation_tolerance.nanoseconds < 0) {
        throw std::invalid_argument(
            "Audio clock discipline tolerances cannot be negative.");
    }
}

AudioClockDisciplineDecision AudioClockDiscipline::evaluate(
    const AudioOutputPosition& observation,
    const PlaybackClockSnapshot& playback,
    MonotonicTime now) const noexcept {
    if (playback.state != PlaybackClockState::Running) {
        return ignored(
            AudioClockDisciplineReason::PlaybackNotRunning,
            observation.revision);
    }
    if (observation.source != AudioOutputPositionSource::HardwareClock) {
        return ignored(
            AudioClockDisciplineReason::HardwareClockUnavailable,
            observation.revision);
    }
    if (observation.presented_until.nanoseconds < 0) {
        return ignored(
            AudioClockDisciplineReason::InvalidMediaPosition,
            observation.revision);
    }

    std::uint64_t age{};
    if (observation.sampled_at.nanoseconds > now.nanoseconds) {
        const auto future = observation.sampled_at.nanoseconds - now.nanoseconds;
        if (future > nonnegative_duration(
                config_.future_observation_tolerance)) {
            return ignored(
                AudioClockDisciplineReason::FutureObservation,
                observation.revision);
        }
    } else {
        age = now.nanoseconds - observation.sampled_at.nanoseconds;
        if (age > nonnegative_duration(config_.maximum_observation_age)) {
            return ignored(
                AudioClockDisciplineReason::StaleObservation,
                observation.revision);
        }
    }

    const MediaTime audio_position_now{
        saturating_add_nonnegative(
            observation.presented_until.nanoseconds,
            age)
    };
    const MediaTime drift{
        saturating_subtract(
            audio_position_now.nanoseconds,
            playback.position.nanoseconds)
    };
    const auto threshold = nonnegative_duration(config_.rebase_threshold);

    if (magnitude(drift.nanoseconds) <= threshold) {
        return {
            AudioClockDisciplineAction::Maintain,
            AudioClockDisciplineReason::WithinThreshold,
            audio_position_now,
            drift,
            observation.revision
        };
    }

    return {
        AudioClockDisciplineAction::Rebase,
        AudioClockDisciplineReason::DriftExceeded,
        audio_position_now,
        drift,
        observation.revision
    };
}

} // namespace lapis::media
