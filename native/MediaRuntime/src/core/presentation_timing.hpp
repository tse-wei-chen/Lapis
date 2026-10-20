#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace bdmv::core {

// Pure scheduling policy. No window, audio, FFmpeg, OS timer, or sleep dependencies.
// The caller passes the current media clock in nanoseconds. Clock ownership belongs
// to the presenter (audio-hardware clock when available; otherwise steady fallback).
enum class VideoDisposition : std::uint8_t { Wait, Present, Drop };
struct VideoTimingDecision final {
    VideoDisposition action{VideoDisposition::Present};
    std::int64_t wait_ns{0};
};
class VideoTiming final {
public:
    static constexpr std::int64_t kEarlyToleranceNs = 2'000'000; // 2 ms
    static constexpr std::int64_t kDropThresholdNs = 80'000'000; // 80 ms

    [[nodiscard]] static VideoTimingDecision decide(std::int64_t frame_pts_ns,
                                                    std::int64_t clock_ns) noexcept {
        if (frame_pts_ns < 0 || clock_ns < 0) return {};
        if (frame_pts_ns >= clock_ns) {
            const auto early = static_cast<std::uint64_t>(frame_pts_ns) -
                               static_cast<std::uint64_t>(clock_ns);
            if (early > static_cast<std::uint64_t>(kEarlyToleranceNs)) {
                return {VideoDisposition::Wait, static_cast<std::int64_t>(
                    std::min<std::uint64_t>(early, 1'000'000'000ULL))};
            }
            return {};
        }
        const auto late = static_cast<std::uint64_t>(clock_ns) -
                          static_cast<std::uint64_t>(frame_pts_ns);
        return late > static_cast<std::uint64_t>(kDropThresholdNs)
            ? VideoTimingDecision{VideoDisposition::Drop, 0} : VideoTimingDecision{};
    }
};

// Approximate *played* PCM media position from an output queue. The queued
// duration is NOT the presentation clock by itself: hardware/backend buffering,
// device latency, and resampling need calibrated feedback in production.
// This is a diagnostics estimate only, not sample-accurate lip-sync proof.
[[nodiscard]] inline std::int64_t estimated_audio_playhead_ns(
    std::int64_t last_queued_audio_end_ns,
    std::uint64_t queued_bytes,
    std::uint32_t sample_rate,
    std::uint32_t channels,
    std::uint32_t bytes_per_sample) noexcept {
    if (last_queued_audio_end_ns < 0 || !sample_rate || !channels || !bytes_per_sample)
        return -1;
    const auto bytes_per_frame = static_cast<std::uint64_t>(channels) * bytes_per_sample;
    const auto frames = queued_bytes / bytes_per_frame;
    const auto seconds = frames / sample_rate;
    if (seconds >= static_cast<std::uint64_t>(last_queued_audio_end_ns) / 1'000'000'000ULL + 1ULL)
        return 0;
    const auto queued_ns = seconds * 1'000'000'000ULL +
        ((frames % sample_rate) * 1'000'000'000ULL) / sample_rate;
    return queued_ns >= static_cast<std::uint64_t>(last_queued_audio_end_ns)
        ? 0 : last_queued_audio_end_ns - static_cast<std::int64_t>(queued_ns);
}

} // namespace bdmv::core
