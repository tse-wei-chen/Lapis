#pragma once

#if defined(_WIN32)

#include "core/media_time.hpp"
#include "core/monotonic_time.hpp"

#include <cstdint>
#include <limits>

namespace lapis::media::wasapi {

[[nodiscard]] inline std::uint64_t scale_clock_units(
    std::uint64_t value,
    std::uint64_t frequency,
    std::uint64_t units_per_second) noexcept {
    if (frequency == 0) {
        return 0;
    }

    const auto whole_seconds = value / frequency;
    const auto remainder = value % frequency;
    if (whole_seconds >
        std::numeric_limits<std::uint64_t>::max() / units_per_second) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    auto scaled_remainder = std::uint64_t{};
    if (remainder != 0) {
        if (remainder <=
            std::numeric_limits<std::uint64_t>::max() / units_per_second) {
            scaled_remainder =
                (remainder * units_per_second) / frequency;
        } else {
            const auto ratio = static_cast<long double>(remainder) /
                static_cast<long double>(frequency);
            scaled_remainder = static_cast<std::uint64_t>(
                ratio * static_cast<long double>(units_per_second));
        }
    }

    const auto whole = whole_seconds * units_per_second;
    if (whole > std::numeric_limits<std::uint64_t>::max() - scaled_remainder) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole + scaled_remainder;
}

[[nodiscard]] inline MediaTime media_time_from_audio_clock(
    MediaTime stream_origin,
    std::uint64_t device_position,
    std::uint64_t device_frequency) noexcept {
    constexpr auto nanoseconds_per_second = std::uint64_t{1'000'000'000};
    const auto offset = scale_clock_units(
        device_position,
        device_frequency,
        nanoseconds_per_second);

    if (offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return MediaTime{std::numeric_limits<std::int64_t>::max()};
    }
    const auto signed_offset = static_cast<std::int64_t>(offset);
    if (stream_origin.nanoseconds >
        std::numeric_limits<std::int64_t>::max() - signed_offset) {
        return MediaTime{std::numeric_limits<std::int64_t>::max()};
    }
    return MediaTime{stream_origin.nanoseconds + signed_offset};
}

// IAudioClock::GetPosition returns QPC correlation in 100-nanosecond units.
// Bridge that timestamp into Lapis' monotonic domain by comparing it with a
// current QPC reading captured beside SteadyClockSource::now().
[[nodiscard]] inline MonotonicTime correlate_qpc_100ns_to_monotonic(
    std::uint64_t sampled_qpc_100ns,
    std::uint64_t current_qpc_100ns,
    MonotonicTime current_monotonic) noexcept {
    constexpr auto nanoseconds_per_qpc_unit = std::uint64_t{100};

    if (current_qpc_100ns >= sampled_qpc_100ns) {
        const auto delta_units = current_qpc_100ns - sampled_qpc_100ns;
        const auto maximum_delta =
            std::numeric_limits<std::uint64_t>::max() /
            nanoseconds_per_qpc_unit;
        const auto delta_ns = delta_units > maximum_delta
            ? std::numeric_limits<std::uint64_t>::max()
            : delta_units * nanoseconds_per_qpc_unit;
        return MonotonicTime{
            delta_ns >= current_monotonic.nanoseconds
                ? 0
                : current_monotonic.nanoseconds - delta_ns
        };
    }

    const auto delta_units = sampled_qpc_100ns - current_qpc_100ns;
    const auto maximum_delta =
        std::numeric_limits<std::uint64_t>::max() /
        nanoseconds_per_qpc_unit;
    const auto delta_ns = delta_units > maximum_delta
        ? std::numeric_limits<std::uint64_t>::max()
        : delta_units * nanoseconds_per_qpc_unit;
    return MonotonicTime{
        delta_ns > std::numeric_limits<std::uint64_t>::max() -
            current_monotonic.nanoseconds
            ? std::numeric_limits<std::uint64_t>::max()
            : current_monotonic.nanoseconds + delta_ns
    };
}

} // namespace lapis::media::wasapi

#endif
