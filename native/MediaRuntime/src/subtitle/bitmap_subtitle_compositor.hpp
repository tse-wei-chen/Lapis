#pragma once

#include "core/media_time.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lapis::media {

// Native-owned, straight-alpha RGBA pixels in display-canvas coordinates.
// FFmpeg's palette, packet and AVSubtitleRect storage MUST be copied/converted
// by an adapter before producing this Core contract.
struct BitmapSubtitleRegion final {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba; // contiguous, four bytes per pixel
};

struct BitmapSubtitleCue final {
    MediaTime start{};
    MediaTime end{};
    std::uint32_t canvas_width{};
    std::uint32_t canvas_height{};
    std::vector<BitmapSubtitleRegion> regions;
};

// Borrowed destination. The caller owns the buffer and guarantees exclusive
// access during composition; stride may include padding after each row.
struct BitmapSubtitleSurface final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::size_t stride{};
    std::span<std::uint8_t> rgba;
};

class BitmapSubtitleCompositor final {
public:
    // No allocation and no partial writes on malformed input. Alpha blending
    // uses straight-alpha source-over, preserving transparency in the target.
    // Target and cue must use the same canvas size; scaling belongs to the
    // renderer/color-management boundary, not Core.
    [[nodiscard]] static bool composite(
        const BitmapSubtitleCue& cue, BitmapSubtitleSurface target) noexcept;
};

} // namespace lapis::media
