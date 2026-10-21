#include "subtitle/bitmap_subtitle_compositor.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

int main() {
    using namespace lapis::media;

    BitmapSubtitleCue cue{
        MediaTime{1}, MediaTime{2}, 3, 2,
        {BitmapSubtitleRegion{1, 0, 2, 1, {
            255, 0, 0, 128,  0, 255, 0, 255
        }}}
    };
    // Padded rows must not be overwritten. Existing opaque pixels remain
    // opaque after alpha compositing, and the green source is fully opaque.
    std::vector<std::uint8_t> pixels(32, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            const auto index = static_cast<std::size_t>(y * 16 + x * 4);
            pixels[index + 2] = 255; // opaque blue
            pixels[index + 3] = 255;
        }
        for (int i = 12; i < 16; ++i) pixels[static_cast<std::size_t>(y * 16 + i)] = 77;
    }
    const BitmapSubtitleSurface surface{3, 2, 16, pixels};
    CHECK(BitmapSubtitleCompositor::composite(cue, surface));
    CHECK((std::array<std::uint8_t, 4>{pixels[4], pixels[5], pixels[6], pixels[7]} ==
           std::array<std::uint8_t, 4>{128, 0, 127, 255}));
    CHECK((std::array<std::uint8_t, 4>{pixels[8], pixels[9], pixels[10], pixels[11]} ==
           std::array<std::uint8_t, 4>{0, 255, 0, 255}));
    CHECK(pixels[12] == 77 && pixels[28] == 77);
    CHECK(pixels[19] == 255); // second row stays blue

    // A transparent destination receives the source's straight RGB+alpha.
    std::array<std::uint8_t, 4> transparent{};
    const BitmapSubtitleCue semi{
        MediaTime{0}, MediaTime{10}, 1, 1,
        {BitmapSubtitleRegion{0, 0, 1, 1, {20, 80, 160, 64}}}
    };
    CHECK(BitmapSubtitleCompositor::composite(
        semi, BitmapSubtitleSurface{1, 1, 4, transparent}));
    CHECK((transparent == std::array<std::uint8_t, 4>{20, 80, 160, 64}));

    // Negative coordinates and rectangles outside the canvas are clipped.
    std::array<std::uint8_t, 16> clipped{};
    const BitmapSubtitleCue crop{
        MediaTime{0}, MediaTime{10}, 2, 2,
        {BitmapSubtitleRegion{-1, -1, 2, 2, {
            1, 1, 1, 255,  2, 2, 2, 255,
            3, 3, 3, 255,  9, 8, 7, 255
        }}}
    };
    CHECK(BitmapSubtitleCompositor::composite(
        crop, BitmapSubtitleSurface{2, 2, 8, clipped}));
    CHECK((std::array<std::uint8_t, 4>{clipped[0], clipped[1], clipped[2], clipped[3]} ==
           std::array<std::uint8_t, 4>{9, 8, 7, 255}));
    CHECK(clipped[4] == 0 && clipped[8] == 0);

    // All input is validated before the first write; a bad second rect
    // must not leave partially rendered pixels on a client surface.
    auto bad = semi;
    bad.regions.push_back(BitmapSubtitleRegion{0, 0, 2, 2, {1, 2, 3}});
    std::array<std::uint8_t, 4> unchanged{7, 8, 9, 10};
    CHECK(!BitmapSubtitleCompositor::composite(
        bad, BitmapSubtitleSurface{1, 1, 4, unchanged}));
    CHECK((unchanged == std::array<std::uint8_t, 4>{7, 8, 9, 10}));
    bad = semi;
    bad.end = bad.start;
    CHECK(!BitmapSubtitleCompositor::composite(
        bad, BitmapSubtitleSurface{1, 1, 4, unchanged}));
    bad = semi;
    bad.regions[0].width = std::numeric_limits<std::uint32_t>::max();
    bad.regions[0].height = std::numeric_limits<std::uint32_t>::max();
    CHECK(!BitmapSubtitleCompositor::composite(
        bad, BitmapSubtitleSurface{1, 1, 4, unchanged}));
    bad = semi;
    CHECK(!BitmapSubtitleCompositor::composite(
        bad, BitmapSubtitleSurface{1, 1, 3, unchanged}));
    CHECK(!BitmapSubtitleCompositor::composite(
        bad, BitmapSubtitleSurface{1, 1, 4, std::span<std::uint8_t>(unchanged.data(), 3)}));

    // Invisible pixels and fully off-canvas regions do not change the image.
    auto invisible = semi;
    invisible.regions[0].rgba = {255, 0, 0, 0};
    CHECK(BitmapSubtitleCompositor::composite(
        invisible, BitmapSubtitleSurface{1, 1, 4, unchanged}));
    CHECK((unchanged == std::array<std::uint8_t, 4>{7, 8, 9, 10}));
    invisible.regions[0].x = 25;
    invisible.regions[0].rgba = {255, 0, 0, 255};
    CHECK(BitmapSubtitleCompositor::composite(
        invisible, BitmapSubtitleSurface{1, 1, 4, unchanged}));
    CHECK((unchanged == std::array<std::uint8_t, 4>{7, 8, 9, 10}));
    CHECK(!BitmapSubtitleCompositor::composite(
        semi, BitmapSubtitleSurface{2, 1, 8, unchanged}));

    // Zero-region clear cues are legal, but never change output pixels.
    const BitmapSubtitleCue clear{MediaTime{0}, MediaTime{1}, 1, 1, {}};
    CHECK(BitmapSubtitleCompositor::composite(
        clear, BitmapSubtitleSurface{1, 1, 4, unchanged}));
    CHECK((unchanged == std::array<std::uint8_t, 4>{7, 8, 9, 10}));
    std::cout << "Bitmap RGBA subtitle composition/validation passed\n";
    return 0;
}
