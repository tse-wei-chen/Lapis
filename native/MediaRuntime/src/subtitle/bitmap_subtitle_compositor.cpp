#include "subtitle/bitmap_subtitle_compositor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace lapis::media {
namespace {
constexpr std::uint64_t max_region_bytes = 64ULL * 1024 * 1024;
constexpr std::size_t max_regions = 64;
constexpr std::uint64_t bytes_per_pixel = 4;

bool valid_region(const BitmapSubtitleRegion& region) noexcept {
    if (region.width == 0 || region.height == 0 ||
        region.height > max_region_bytes / bytes_per_pixel / region.width)
        return false;
    return region.rgba.size() ==
        static_cast<std::size_t>(region.width) * region.height * bytes_per_pixel;
}

bool valid_cue(const BitmapSubtitleCue& cue) noexcept {
    if (cue.end <= cue.start || cue.start.nanoseconds < 0 ||
        cue.canvas_width == 0 || cue.canvas_height == 0 ||
        cue.regions.size() > max_regions)
        return false;
    std::uint64_t bytes = 0;
    for (const auto& region : cue.regions) {
        if (!valid_region(region)) return false;
        const auto count = static_cast<std::uint64_t>(region.rgba.size());
        if (count > max_region_bytes - bytes) return false;
        bytes += count;
    }
    return true;
}

bool valid_surface(const BitmapSubtitleCue& cue,
                   const BitmapSubtitleSurface& target) noexcept {
    if (target.width != cue.canvas_width ||
        target.height != cue.canvas_height ||
        static_cast<std::uint64_t>(target.width) * bytes_per_pixel >
            std::numeric_limits<std::size_t>::max())
        return false;
    const auto row_bytes = static_cast<std::size_t>(target.width) * bytes_per_pixel;
    if (target.stride < row_bytes || target.height == 0) return false;
    // Check the last pixel of the last row without overflowing size_t.
    const auto last_row = static_cast<std::size_t>(target.height - 1);
    if (last_row > (std::numeric_limits<std::size_t>::max() - row_bytes) /
                       target.stride)
        return false;
    return target.rgba.size() >= last_row * target.stride + row_bytes;
}

void over(std::uint8_t* destination, const std::uint8_t* source) noexcept {
    const auto alpha = static_cast<std::uint32_t>(source[3]);
    if (alpha == 0) return;
    if (alpha == 255) {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        destination[3] = 255;
        return;
    }
    const auto dest_alpha = static_cast<std::uint32_t>(destination[3]);
    const auto inverse = 255U - alpha;
    const auto alpha_numerator = alpha * 255U + dest_alpha * inverse;
    if (alpha_numerator == 0) return;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto color_numerator =
            static_cast<std::uint32_t>(source[channel]) * alpha * 255U +
            static_cast<std::uint32_t>(destination[channel]) * dest_alpha * inverse;
        destination[channel] = static_cast<std::uint8_t>(
            (color_numerator + alpha_numerator / 2U) / alpha_numerator);
    }
    destination[3] = static_cast<std::uint8_t>((alpha_numerator + 127U) / 255U);
}
} // namespace

bool BitmapSubtitleCompositor::composite(
    const BitmapSubtitleCue& cue, BitmapSubtitleSurface target) noexcept {
    if (!valid_cue(cue) || !valid_surface(cue, target)) return false;

    for (const auto& region : cue.regions) {
        const auto left = std::max<std::int64_t>(0, region.x);
        const auto top = std::max<std::int64_t>(0, region.y);
        const auto right = std::min<std::int64_t>(
            cue.canvas_width, static_cast<std::int64_t>(region.x) + region.width);
        const auto bottom = std::min<std::int64_t>(
            cue.canvas_height, static_cast<std::int64_t>(region.y) + region.height);
        if (left >= right || top >= bottom) continue;

        for (auto y = top; y < bottom; ++y) {
            const auto row = static_cast<std::size_t>(y) * target.stride;
            const auto source_row = static_cast<std::size_t>(y - region.y) *
                region.width * bytes_per_pixel;
            for (auto x = left; x < right; ++x) {
                const auto dest_offset = row + static_cast<std::size_t>(x) * bytes_per_pixel;
                const auto source_offset = source_row +
                    static_cast<std::size_t>(x - region.x) * bytes_per_pixel;
                over(target.rgba.data() + dest_offset,
                     region.rgba.data() + source_offset);
            }
        }
    }
    return true;
}

} // namespace lapis::media
