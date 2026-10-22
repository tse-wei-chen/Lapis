#include "video/x11/xcb_software_video_presenter.hpp"

#if !defined(__linux__)
#error XcbSoftwareVideoPresenter is a Linux X11/XWayland adapter.
#endif

#include <xcb/xcb.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace lapis::media::x11 {
namespace {

std::uint32_t channel_bits(std::uint8_t value, std::uint32_t mask) noexcept {
    if (mask == 0) return 0;
    const auto count = std::popcount(mask);
    const auto maximum = (std::uint64_t{1} << count) - 1;
    const auto scaled = (static_cast<std::uint64_t>(value) * maximum + 127) / 255;
    return static_cast<std::uint32_t>(
        (scaled << std::countr_zero(mask)) & mask);
}

} // namespace

class XcbSoftwareVideoPresenter::Impl final {
public:
    ~Impl() { detach(); }

    [[nodiscard]] VideoOutputSurfaceApplyStatus bind(
        VideoOutputSurfaceDescriptor surface) {
        if (!valid_video_output_surface(surface))
            return VideoOutputSurfaceApplyStatus::Failed;
        if (surface.kind == VideoOutputSurfaceKind::None) {
            detach();
            return VideoOutputSurfaceApplyStatus::Ready;
        }
        if (surface.kind != VideoOutputSurfaceKind::X11Window)
            return VideoOutputSurfaceApplyStatus::Unsupported;
        if (connection_ != nullptr && window_ == surface.native_handle &&
            xcb_connection_has_error(connection_) == 0)
            return VideoOutputSurfaceApplyStatus::Ready;

        // Detach is an acknowledgement: no subsequent render operation can
        // touch the previously borrowed window after bind() returns.
        detach();
        int screen_index{};
        connection_ = xcb_connect(nullptr, &screen_index);
        if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
            detach();
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }
        window_ = static_cast<xcb_window_t>(surface.native_handle);
        xcb_generic_error_t* error{};
        auto* geometry = xcb_get_geometry_reply(
            connection_, xcb_get_geometry(connection_, window_), &error);
        if (error != nullptr || geometry == nullptr) {
            std::free(error);
            std::free(geometry);
            detach();
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }
        depth_ = geometry->depth;
        std::free(geometry);

        auto* attributes = xcb_get_window_attributes_reply(
            connection_, xcb_get_window_attributes(connection_, window_), &error);
        if (error != nullptr || attributes == nullptr) {
            std::free(error);
            std::free(attributes);
            detach();
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }
        const auto visual_id = attributes->visual;
        std::free(attributes);

        const auto* setup = xcb_get_setup(connection_);
        const auto formats = xcb_setup_pixmap_formats_iterator(setup);
        bool supported_format = false;
        for (auto fmt = formats; fmt.rem != 0; xcb_format_next(&fmt)) {
            if (fmt.data->depth == depth_ && fmt.data->bits_per_pixel == 32) {
                supported_format = true;
                break;
            }
        }

        auto screen = xcb_setup_roots_iterator(setup);
        for (int i = 0; i < screen_index && screen.rem != 0; ++i)
            xcb_screen_next(&screen);
        bool found_visual = false;
        for (auto depths = screen.rem != 0
                ? xcb_screen_allowed_depths_iterator(screen.data)
                : xcb_depth_iterator_t{}; depths.rem != 0;
                xcb_depth_next(&depths)) {
            for (auto visuals = xcb_depth_visuals_iterator(depths.data);
                 visuals.rem != 0; xcb_visualtype_next(&visuals)) {
                if (visuals.data->visual_id != visual_id) continue;
                if (visuals.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR) continue;
                red_ = visuals.data->red_mask;
                green_ = visuals.data->green_mask;
                blue_ = visuals.data->blue_mask;
                found_visual = red_ != 0 && green_ != 0 && blue_ != 0;
            }
        }
        if (!supported_format || !found_visual) {
            detach();
            return VideoOutputSurfaceApplyStatus::Unsupported;
        }

        little_endian_ = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
        gc_ = xcb_generate_id(connection_);
        const auto cookie = xcb_create_gc_checked(
            connection_, gc_, window_, 0, nullptr);
        error = xcb_request_check(connection_, cookie);
        if (error != nullptr) {
            std::free(error);
            detach();
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }
        return VideoOutputSurfaceApplyStatus::Ready;
    }

    void detach() noexcept {
        pixels_.clear();
        if (connection_ != nullptr) {
            if (gc_ != XCB_NONE) xcb_free_gc(connection_, gc_);
            xcb_disconnect(connection_);
        }
        connection_ = nullptr;
        gc_ = XCB_NONE;
        window_ = XCB_NONE;
        depth_ = 0;
        red_ = green_ = blue_ = 0;
    }

    [[nodiscard]] VideoPresentStatus draw(const VideoFrame& frame) {
        if (connection_ == nullptr) {
            // Unbound sessions intentionally remain headless.
            return VideoPresentStatus::Presented;
        }
        if (xcb_connection_has_error(connection_) != 0)
            return VideoPresentStatus::DeviceLost;
        if (frame.surface == nullptr ||
            frame.surface->kind() != VideoSurfaceKind::SystemMemory ||
            frame.surface->pixel_format() != VideoPixelFormat::Bgra8)
            return VideoPresentStatus::UnsupportedSurface;
        const auto* source = dynamic_cast<const ISystemMemoryVideoSurface*>(
            frame.surface.get());
        if (source == nullptr || source->plane_count() != 1 ||
            source->width() == 0 || source->height() == 0 ||
            source->width() > std::numeric_limits<std::uint32_t>::max() / 4)
            return VideoPresentStatus::UnsupportedSurface;
        const auto plane = source->plane(0);
        const auto row_bytes = static_cast<std::uint64_t>(source->width()) * 4;
        if (plane.data == nullptr || plane.stride <= 0 ||
            static_cast<std::uint64_t>(plane.stride) < row_bytes)
            return VideoPresentStatus::UnsupportedSurface;

        xcb_generic_error_t* error{};
        auto* geometry = xcb_get_geometry_reply(
            connection_, xcb_get_geometry(connection_, window_), &error);
        if (error != nullptr || geometry == nullptr) {
            std::free(error);
            std::free(geometry);
            return VideoPresentStatus::DeviceLost;
        }
        const auto width = static_cast<std::uint32_t>(geometry->width);
        const auto height = static_cast<std::uint32_t>(geometry->height);
        const auto current_depth = geometry->depth;
        std::free(geometry);
        if (current_depth != depth_) return VideoPresentStatus::DeviceLost;
        if (width == 0 || height == 0) return VideoPresentStatus::Presented;
        // xcb_put_image is bounded by 16-bit dimensions/coordinates and a
        // standard request payload. Refuse pathological window sizes.
        if (width > 8192 || height > 8192)
            return VideoPresentStatus::UnsupportedSurface;
        const auto image_bytes = static_cast<std::uint64_t>(width) * height * 4;
        // CPU fallback must not allocate unbounded storage on window resize.
        if (image_bytes > 128ULL * 1024 * 1024)
            return VideoPresentStatus::UnsupportedSurface;

        pixels_.assign(static_cast<std::size_t>(image_bytes), 0);
        const auto scale = std::min(
            static_cast<double>(width) / source->width(),
            static_cast<double>(height) / source->height());
        const auto viewport_w = std::min(width, std::max(1U, static_cast<std::uint32_t>(
            source->width() * scale)));
        const auto viewport_h = std::min(height, std::max(1U, static_cast<std::uint32_t>(
            source->height() * scale)));
        const auto offset_x = (width - viewport_w) / 2;
        const auto offset_y = (height - viewport_h) / 2;
        for (std::uint32_t y = 0; y < viewport_h; ++y) {
            const auto src_y = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(y) * source->height() / viewport_h);
            const auto* src_row = plane.data +
                static_cast<std::ptrdiff_t>(src_y) * plane.stride;
            for (std::uint32_t x = 0; x < viewport_w; ++x) {
                const auto src_x = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(x) * source->width() / viewport_w);
                const auto* bgra = reinterpret_cast<const std::uint8_t*>(
                    src_row + static_cast<std::size_t>(src_x) * 4);
                const auto pixel = channel_bits(bgra[2], red_) |
                    channel_bits(bgra[1], green_) |
                    channel_bits(bgra[0], blue_);
                auto* destination = pixels_.data() +
                    (static_cast<std::size_t>(y + offset_y) * width +
                     x + offset_x) * 4;
                for (unsigned byte = 0; byte < 4; ++byte) {
                    destination[byte] = static_cast<std::uint8_t>(
                        pixel >> (little_endian_ ? byte * 8 : (3 - byte) * 8));
                }
            }
        }

        // A normal X11 core request has a ~256 KiB payload ceiling. Split into
        // bounded bands (including 4K windows) rather than silently exceeding
        // the server's maximum request length.
        const auto stride = static_cast<std::size_t>(width) * 4;
        const auto band_rows = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(60 * 1024 / stride));
        for (std::uint32_t y = 0; y < height; y += band_rows) {
            const auto rows = std::min(height - y, band_rows);
            const auto cookie = xcb_put_image_checked(
                connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, gc_,
                static_cast<std::uint16_t>(width),
                static_cast<std::uint16_t>(rows),
                0, static_cast<std::int16_t>(y), 0, depth_,
                static_cast<std::uint32_t>(stride * rows),
                pixels_.data() + static_cast<std::size_t>(y) * stride);
            error = xcb_request_check(connection_, cookie);
            if (error != nullptr) {
                std::free(error);
                return VideoPresentStatus::DeviceLost;
            }
        }
        xcb_flush(connection_);
        return xcb_connection_has_error(connection_) == 0
            ? VideoPresentStatus::Presented
            : VideoPresentStatus::DeviceLost;
    }

private:
    xcb_connection_t* connection_{};
    xcb_window_t window_{XCB_NONE};
    xcb_gcontext_t gc_{XCB_NONE};
    std::uint8_t depth_{};
    std::uint32_t red_{}, green_{}, blue_{};
    bool little_endian_{true};
    std::vector<std::uint8_t> pixels_;
};

XcbSoftwareVideoPresenter::XcbSoftwareVideoPresenter()
    : impl_(std::make_unique<Impl>()) {}
XcbSoftwareVideoPresenter::~XcbSoftwareVideoPresenter() = default;

VideoOutputSurfaceApplyStatus XcbSoftwareVideoPresenter::set_output_surface(
    const VideoOutputSurfaceDescriptor& surface) {
    return impl_->bind(surface);
}

VideoPresenterBeginStatus XcbSoftwareVideoPresenter::begin(PlaybackEpoch) {
    return VideoPresenterBeginStatus::Ready;
}

VideoPresentStatus XcbSoftwareVideoPresenter::present(
    const VideoFrame& frame, const VideoPresentContext&) {
    return impl_->draw(frame);
}

void XcbSoftwareVideoPresenter::end() noexcept {
    // Detach is an explicit, independent lifetime boundary; a stopped
    // generation must not destroy the host-owned X11 window.
}

} // namespace lapis::media::x11
