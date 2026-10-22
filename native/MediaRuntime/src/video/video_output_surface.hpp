#pragma once

#include <cstdint>

namespace lapis::media {

enum class VideoOutputSurfaceKind : std::uint8_t {
    None,
    Win32Window,
    X11Window
};

struct VideoOutputSurfaceDescriptor final {
    VideoOutputSurfaceKind kind{VideoOutputSurfaceKind::None};
    std::uintptr_t native_handle{};

    friend bool operator==(
        const VideoOutputSurfaceDescriptor&,
        const VideoOutputSurfaceDescriptor&) = default;
};

[[nodiscard]] constexpr bool valid_video_output_surface(
    VideoOutputSurfaceDescriptor surface) noexcept {
    switch (surface.kind) {
        case VideoOutputSurfaceKind::None:
            return surface.native_handle == 0;
        case VideoOutputSurfaceKind::Win32Window:
            return surface.native_handle != 0;
        case VideoOutputSurfaceKind::X11Window:
            // X11 Window is a 32-bit server-side XID, not a pointer.
            return surface.native_handle != 0 &&
                surface.native_handle <= UINT32_MAX;
    }

    return false;
}

enum class VideoOutputSurfaceApplyStatus : std::uint8_t {
    Ready,
    Unsupported,
    Unavailable,
    Failed
};

enum class VideoOutputSurfaceControlStatus : std::uint8_t {
    Applied,
    Deferred,
    Invalid,
    Unsupported,
    Unavailable,
    Failed
};

} // namespace lapis::media
