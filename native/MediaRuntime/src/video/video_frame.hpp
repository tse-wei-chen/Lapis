#pragma once

#include "core/media_time.hpp"
#include "core/rational.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace lapis::media {

enum class VideoSurfaceKind : std::uint8_t {
    SystemMemory,
    D3D11Texture,
    D3D12Resource
};

enum class VideoPixelFormat : std::uint8_t {
    Unknown,
    Gray8,
    Yuv420P,
    Yuv422P,
    Yuv444P,
    Nv12,
    P010,
    Bgra8,
    Rgba8,
    Rgb24
};

struct VideoPlaneView final {
    const std::byte* data{};
    std::ptrdiff_t stride{};
};

class IVideoSurface {
public:
    virtual ~IVideoSurface() = default;

    [[nodiscard]] virtual VideoSurfaceKind kind() const noexcept = 0;
    [[nodiscard]] virtual VideoPixelFormat pixel_format() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
};

class ISystemMemoryVideoSurface : public IVideoSurface {
public:
    [[nodiscard]] VideoSurfaceKind kind() const noexcept final {
        return VideoSurfaceKind::SystemMemory;
    }

    [[nodiscard]] virtual std::uint32_t plane_count() const noexcept = 0;
    [[nodiscard]] virtual VideoPlaneView plane(std::uint32_t index) const noexcept = 0;
};

struct VideoFrame final {
    std::shared_ptr<IVideoSurface> surface;
    MediaTime timestamp{};
    std::optional<SourceTimestamp> source_timestamp;
};

} // namespace lapis::media
