#pragma once
#include <cstdint>
#include <memory>
#include <string_view>

// Internal C++ design contracts, not exported C ABI and not implemented yet.
// No OS handles, FFmpeg types, libbluray types, or frame buffer ownership in UI.
namespace bdmv::core {

struct MediaTime final { std::int64_t nanoseconds{0}; };
struct SourceTimestamp final { std::int64_t ticks{0}; std::int32_t numerator{1}; std::int32_t denominator{1}; };
struct PlaybackEpoch final { std::uint64_t value{0}; };
struct PlaybackSessionId final { std::uint64_t value{0}; };

enum class SurfaceKind : std::uint32_t { CpuMemory, GpuOpaque };
enum class Transfer : std::uint32_t { Unknown, Bt1886, Srgb, Pq, Hlg };
enum class Primaries : std::uint32_t { Unknown, Bt709, Bt2020, P3D65 };
enum class Matrix : std::uint32_t { Unknown, Bt709, Bt2020Ncl, Identity };
enum class Range : std::uint32_t { Limited, Full };

struct ColorMetadata final {
    Primaries primaries{Primaries::Unknown};
    Transfer transfer{Transfer::Unknown};
    Matrix matrix{Matrix::Unknown};
    Range range{Range::Limited};
    // HDR mastering metadata + dynamic side data require separately versioned DTOs.
};

struct FrameInfo final {
    MediaTime pts;
    PlaybackEpoch epoch;
    std::uint32_t width{0};
    std::uint32_t height{0};
    ColorMetadata color;
};

// Lease pins resource lifetime until presenter returns it to the decoder's pool.
// Actual DMA-BUF / DXGI / Vulkan handles are owned by the platform adapter.
class IVideoSurface {
public:
    virtual ~IVideoSurface() = default;
    [[nodiscard]] virtual SurfaceKind kind() const noexcept = 0;
};

struct VideoFrame final {
    FrameInfo info;
    std::shared_ptr<IVideoSurface> surface;
};

// Platform's window provider owns native handles. The core only sees a token.
struct ViewportToken final { std::uint64_t id{0}; };
struct PresenterCapabilities final {
    bool hdr_passthrough{false};
    bool hdr_to_sdr_tonemap{false};
    bool zero_copy_compatible{false};
};

enum class OutputResult : std::uint32_t {
    Ok, Unsupported, SurfaceLost, DeviceLost, Backpressure, Fatal
};

class IVideoPresenter {
public:
    virtual ~IVideoPresenter() = default;
    [[nodiscard]] virtual PresenterCapabilities capabilities() const noexcept = 0;
    virtual OutputResult attach(ViewportToken viewport) noexcept = 0;
    virtual OutputResult present(const VideoFrame& frame) noexcept = 0;
    virtual void detach() noexcept = 0;
};

struct AudioFormat final {
    std::uint32_t sample_rate{0};
    std::uint32_t channels{0};
    std::uint64_t layout_mask{0};
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    // PCM sample format, clock, latency & bitstream profile are separate contracts in ADR-0004.
    virtual OutputResult start(AudioFormat format) noexcept = 0;
    virtual OutputResult stop() noexcept = 0;
};

struct BackendSelection final {
    std::string_view video;   // e.g. "vulkan", "d3d11", "software"
    std::string_view audio;   // e.g. "pipewire", "wasapi", "null"
    bool prefer_zero_copy{true};
};

// Only adapter factory implementation may include platform-specific headers.
class IPlatformBackendFactory {
public:
    virtual ~IPlatformBackendFactory() = default;
    virtual std::unique_ptr<IVideoPresenter> make_presenter(BackendSelection) = 0;
    virtual std::unique_ptr<IAudioSink> make_audio_sink(BackendSelection) = 0;
};

} // namespace bdmv::core
