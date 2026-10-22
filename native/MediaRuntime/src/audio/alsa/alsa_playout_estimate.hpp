#pragma once

#include "core/media_time.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace lapis::media::alsa {

// ALSA reports queued frames, NOT a hardware-correlated media clock.
// Convert an accepted PCM prefix's media end and its remaining queue depth
// into a conservative playout estimate. Saturation prevents malformed media
// timestamps or device latency from overflowing canonical MediaTime.
[[nodiscard]] inline MediaTime estimate_playout_position(
    MediaTime accepted_end, std::uint64_t queued_frames,
    std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0) return {};
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto latency = static_cast<long double>(queued_frames) *
        1'000'000'000.0L / static_cast<long double>(sample_rate);
    const auto position = static_cast<long double>(
        accepted_end.nanoseconds) - latency;
    return MediaTime{
        position <= 0 ? 0 :
        position >= static_cast<long double>(maximum) ? maximum :
        static_cast<std::int64_t>(position)
    };
}

[[nodiscard]] inline MediaTime accepted_media_end(
    MediaTime frame_timestamp, std::uint64_t submitted_through,
    std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0) return {};
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto position = static_cast<long double>(
        frame_timestamp.nanoseconds) +
        static_cast<long double>(submitted_through) *
            1'000'000'000.0L / static_cast<long double>(sample_rate);
    return MediaTime{
        position <= 0 ? 0 :
        position >= static_cast<long double>(maximum) ? maximum :
        static_cast<std::int64_t>(position)
    };
}

} // namespace lapis::media::alsa
