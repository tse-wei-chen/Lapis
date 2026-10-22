#include "platform/playback_execution_factory.hpp"

#include "core/playback_epoch.hpp"
#include "video/video_frame.hpp"
#include "video/video_output_surface.hpp"
#include "video/video_presenter.hpp"

#include <xcb/xcb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "X11 pixel smoke failed at line %d: %s\\n", \
            __LINE__, #condition); \
        return __LINE__; \
    } \
} while (false)

class TestBgraSurface final : public lapis::media::ISystemMemoryVideoSurface {
public:
    [[nodiscard]] lapis::media::VideoPixelFormat pixel_format() const noexcept override {
        return lapis::media::VideoPixelFormat::Bgra8;
    }
    [[nodiscard]] std::uint32_t width() const noexcept override { return 2; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return 2; }
    [[nodiscard]] std::uint32_t plane_count() const noexcept override { return 1; }
    [[nodiscard]] lapis::media::VideoPlaneView plane(
        std::uint32_t index) const noexcept override {
        return index == 0
            ? lapis::media::VideoPlaneView{
                reinterpret_cast<const std::byte*>(pixels_.data()), 8}
            : lapis::media::VideoPlaneView{};
    }
private:
    // BGRA: red, green, blue, white (top-left to bottom-right).
    std::array<std::uint8_t, 16> pixels_{{
        0, 0, 255, 255, 0, 255, 0, 255,
        255, 0, 0, 255, 255, 255, 255, 255
    }};
};

std::uint32_t get_pixel(const xcb_get_image_reply_t* reply,
                        std::uint32_t width, std::uint32_t x,
                        std::uint32_t y, bool little_endian) {
    const auto* bytes = xcb_get_image_data(reply) + (y * width + x) * 4;
    std::uint32_t result{};
    for (std::uint32_t i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(bytes[i]) <<
            (little_endian ? (i * 8) : ((3 - i) * 8));
    return result;
}

} // namespace

int main() {
    using namespace lapis::media;
    int screen_index{};
    xcb_connection_t* conn = xcb_connect(nullptr, &screen_index);
    CHECK(conn != nullptr && xcb_connection_has_error(conn) == 0);
    auto screen_it = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (int i = 0; i < screen_index; ++i) xcb_screen_next(&screen_it);
    CHECK(screen_it.rem != 0);
    const auto* screen = screen_it.data;
    const auto id = xcb_generate_id(conn);
    CHECK(id != 0);
    const auto create = xcb_create_window_checked(
        conn, XCB_COPY_FROM_PARENT, id, screen->root,
        0, 0, 4, 4, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual, 0, nullptr);
    auto* error = xcb_request_check(conn, create);
    CHECK(error == nullptr);
    const auto map = xcb_map_window_checked(conn, id);
    error = xcb_request_check(conn, map);
    CHECK(error == nullptr);
    xcb_flush(conn);

    auto presenter = platform::create_video_presenter();
    CHECK(presenter != nullptr);
    CHECK(presenter->set_output_surface(
        {VideoOutputSurfaceKind::Win32Window, 42}) ==
        VideoOutputSurfaceApplyStatus::Unsupported);
    CHECK(presenter->set_output_surface(
        {VideoOutputSurfaceKind::X11Window, 0}) ==
        VideoOutputSurfaceApplyStatus::Failed);
    CHECK(presenter->set_output_surface(
        {VideoOutputSurfaceKind::X11Window, id}) ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(presenter->begin(PlaybackEpoch{1}) ==
        VideoPresenterBeginStatus::Ready);
    CHECK(presenter->present(VideoFrame{}, VideoPresentContext{PlaybackEpoch{1}}) ==
        VideoPresentStatus::UnsupportedSurface);

    VideoFrame frame{};
    frame.surface = std::make_shared<TestBgraSurface>();
    CHECK(presenter->present(frame, VideoPresentContext{PlaybackEpoch{1}}) ==
        VideoPresentStatus::Presented);

    // Read actual pixels back from the virtual X server. Merely returning
    // Presented from a headless presenter is not enough to pass this test.
    xcb_generic_error_t* read_error{};
    auto* image = xcb_get_image_reply(conn,
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
            id, 0, 0, 4, 4, ~std::uint32_t{0}),
        &read_error);
    CHECK(read_error == nullptr && image != nullptr);
    CHECK(xcb_get_image_data_length(image) >= 4 * 4 * 4);
    const bool little = xcb_get_setup(conn)->image_byte_order ==
        XCB_IMAGE_ORDER_LSB_FIRST;
    const auto top_left = get_pixel(image, 4, 0, 0, little);
    const auto top_right = get_pixel(image, 4, 3, 0, little);
    const auto bottom_left = get_pixel(image, 4, 0, 3, little);
    const auto bottom_right = get_pixel(image, 4, 3, 3, little);
    std::free(image);
    CHECK((top_left & 0x00ffffffU) == 0x00ff0000U);
    CHECK((top_right & 0x00ffffffU) == 0x0000ff00U);
    CHECK((bottom_left & 0x00ffffffU) == 0x000000ffU);
    CHECK((bottom_right & 0x00ffffffU) == 0x00ffffffU);

    // Real resize reuses the host-owned XID, changes the viewport and adds
    // black pillarboxes rather than stretching a square into a wide frame.
    const std::uint32_t extent[] = {8, 4};
    const auto configure = xcb_configure_window_checked(
        conn, id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
        extent);
    error = xcb_request_check(conn, configure);
    CHECK(error == nullptr);
    xcb_flush(conn);
    CHECK(presenter->present(frame, VideoPresentContext{PlaybackEpoch{1}}) ==
        VideoPresentStatus::Presented);
    image = xcb_get_image_reply(conn,
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
            id, 0, 0, 8, 4, ~std::uint32_t{0}), &read_error);
    CHECK(read_error == nullptr && image != nullptr);
    CHECK(xcb_get_image_data_length(image) >= 8 * 4 * 4);
    CHECK((get_pixel(image, 8, 0, 0, little) & 0x00ffffffU) == 0);
    CHECK((get_pixel(image, 8, 2, 0, little) & 0x00ffffffU) == 0x00ff0000U);
    std::free(image);

    CHECK(presenter->set_output_surface(
        {VideoOutputSurfaceKind::None, 0}) ==
        VideoOutputSurfaceApplyStatus::Ready);
    presenter->end();
    CHECK(presenter->present(frame, VideoPresentContext{PlaybackEpoch{1}}) ==
        VideoPresentStatus::Presented); // Detached; intentionally headless.
    presenter.reset();
    xcb_destroy_window(conn, id);
    xcb_disconnect(conn);
    return 0;
}
