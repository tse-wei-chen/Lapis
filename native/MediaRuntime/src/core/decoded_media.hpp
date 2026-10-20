#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace bdmv::core {
// Portable, OWNED software decode output. No borrowed AVFrame/AVPacket pointers.
// This is an adapter-to-runtime DTO, not the final GPU surface representation.
enum class DecodedKind : std::uint8_t { VideoRgba8, AudioS16Stereo };
struct DecodedBuffer final {
    DecodedKind kind{DecodedKind::VideoRgba8};
    std::int64_t pts_ns{0};
    std::int64_t duration_ns{0};
    // Frames belong to the active session/epoch; DecodePump labels events at publication.
    std::uint64_t session_id{0};
    std::uint64_t epoch{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t sample_rate{0};
    std::uint32_t channels{0};
    // Video: tightly packed RGBA, row stride = width * 4.
    // Audio: 16-bit little-endian, interleaved stereo, fixed 48000 Hz.
    std::vector<std::uint8_t> bytes;
};

// A/V decoder must be distinguishable from EOF and errors, including failed seek.
enum class DecodeReadStatus : std::uint8_t { Frame, EndOfStream, Error };

// Gate events AFTER decoding; a concurrent Seek may change epoch while codec runs.
// The receiving queue will additionally verify epoch at enqueue time.
[[nodiscard]] inline bool matches_epoch(const DecodedBuffer& frame,
                                         std::uint64_t session_id,
                                         std::uint64_t epoch) noexcept {
    return frame.session_id == session_id && frame.epoch == epoch;
}

// Prevent overflow and ridiculous untrusted allocations for software video.
[[nodiscard]] inline std::optional<std::size_t> rgba_buffer_size(std::uint32_t w,
                                                                  std::uint32_t h) noexcept {
    constexpr std::uint32_t kMaxDimension = 16384;
    if (!w || !h || w > kMaxDimension || h > kMaxDimension) return std::nullopt;
    const auto pixels = static_cast<std::uint64_t>(w) * h;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4 ||
        pixels * 4 > 128ULL * 1024 * 1024) return std::nullopt;
    return static_cast<std::size_t>(pixels * 4);
}
} // namespace bdmv::core
