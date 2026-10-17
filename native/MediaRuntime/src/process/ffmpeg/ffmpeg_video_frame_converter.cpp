#include "process/ffmpeg/ffmpeg_video_frame_converter.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace lapis::media::ffmpeg {
namespace {

std::optional<AVPixelFormat> to_av_pixel_format(VideoPixelFormat format) noexcept {
    switch (format) {
        case VideoPixelFormat::Gray8:
            return AV_PIX_FMT_GRAY8;
        case VideoPixelFormat::Yuv420P:
            return AV_PIX_FMT_YUV420P;
        case VideoPixelFormat::Yuv422P:
            return AV_PIX_FMT_YUV422P;
        case VideoPixelFormat::Yuv444P:
            return AV_PIX_FMT_YUV444P;
        case VideoPixelFormat::Nv12:
            return AV_PIX_FMT_NV12;
        case VideoPixelFormat::P010:
            return AV_PIX_FMT_P010LE;
        case VideoPixelFormat::Bgra8:
            return AV_PIX_FMT_BGRA;
        case VideoPixelFormat::Rgba8:
            return AV_PIX_FMT_RGBA;
        case VideoPixelFormat::Rgb24:
            return AV_PIX_FMT_RGB24;
        case VideoPixelFormat::Unknown:
            break;
    }

    return std::nullopt;
}

class ConvertedBgraSurface final : public ISystemMemoryVideoSurface {
public:
    ConvertedBgraSurface(
        std::uint32_t width,
        std::uint32_t height,
        std::array<std::uint8_t*, 4> planes,
        std::array<int, 4> strides) noexcept
        : width_(width),
          height_(height),
          planes_(planes),
          strides_(strides) {}

    ~ConvertedBgraSurface() override {
        av_free(planes_[0]);
        planes_[0] = nullptr;
    }

    [[nodiscard]] VideoPixelFormat pixel_format() const noexcept override {
        return VideoPixelFormat::Bgra8;
    }

    [[nodiscard]] std::uint32_t width() const noexcept override {
        return width_;
    }

    [[nodiscard]] std::uint32_t height() const noexcept override {
        return height_;
    }

    [[nodiscard]] std::uint32_t plane_count() const noexcept override {
        return 1;
    }

    [[nodiscard]] VideoPlaneView plane(std::uint32_t index) const noexcept override {
        if (index != 0 || planes_[0] == nullptr || strides_[0] <= 0) {
            return {};
        }
        return {
            reinterpret_cast<const std::byte*>(planes_[0]),
            static_cast<std::ptrdiff_t>(strides_[0])
        };
    }

private:
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::array<std::uint8_t*, 4> planes_{};
    std::array<int, 4> strides_{};
};

} // namespace

class FfmpegVideoFrameConverter::Impl final {
public:
    ~Impl() {
        sws_freeContext(context_);
    }

    [[nodiscard]] VideoProcessResult process(const VideoFrame& frame) {
        if (frame.surface == nullptr ||
            frame.surface->kind() != VideoSurfaceKind::SystemMemory) {
            return {VideoProcessStatus::UnsupportedInput, std::nullopt};
        }

        const auto* source = dynamic_cast<const ISystemMemoryVideoSurface*>(
            frame.surface.get());
        if (source == nullptr || source->width() == 0 || source->height() == 0) {
            return {VideoProcessStatus::UnsupportedInput, std::nullopt};
        }

        const auto source_format = to_av_pixel_format(source->pixel_format());
        if (!source_format.has_value()) {
            return {VideoProcessStatus::UnsupportedInput, std::nullopt};
        }

        if (source->width() > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max()) ||
            source->height() > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max())) {
            return {VideoProcessStatus::UnsupportedInput, std::nullopt};
        }

        const auto width = static_cast<int>(source->width());
        const auto height = static_cast<int>(source->height());

        context_ = sws_getCachedContext(
            context_,
            width,
            height,
            *source_format,
            width,
            height,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (context_ == nullptr) {
            return {VideoProcessStatus::Failed, std::nullopt};
        }

        std::array<const std::uint8_t*, 4> source_data{};
        std::array<int, 4> source_strides{};
        const auto plane_count = source->plane_count();
        if (plane_count == 0 || plane_count > source_data.size()) {
            return {VideoProcessStatus::UnsupportedInput, std::nullopt};
        }

        for (std::uint32_t index = 0; index < plane_count; ++index) {
            const auto plane = source->plane(index);
            if (plane.data == nullptr ||
                plane.stride < std::numeric_limits<int>::min() ||
                plane.stride > std::numeric_limits<int>::max()) {
                return {VideoProcessStatus::UnsupportedInput, std::nullopt};
            }
            source_data[index] = reinterpret_cast<const std::uint8_t*>(plane.data);
            source_strides[index] = static_cast<int>(plane.stride);
        }

        std::array<std::uint8_t*, 4> destination_planes{};
        std::array<int, 4> destination_strides{};
        const auto allocation_size = av_image_alloc(
            destination_planes.data(),
            destination_strides.data(),
            width,
            height,
            AV_PIX_FMT_BGRA,
            32);
        if (allocation_size < 0 || destination_planes[0] == nullptr) {
            return {VideoProcessStatus::Failed, std::nullopt};
        }

        const auto converted_height = sws_scale(
            context_,
            source_data.data(),
            source_strides.data(),
            0,
            height,
            destination_planes.data(),
            destination_strides.data());
        if (converted_height != height) {
            av_free(destination_planes[0]);
            return {VideoProcessStatus::Failed, std::nullopt};
        }

        try {
            auto surface = std::make_shared<ConvertedBgraSurface>(
                source->width(),
                source->height(),
                destination_planes,
                destination_strides);

            VideoFrame converted{
                std::move(surface),
                frame.timestamp,
                frame.source_timestamp
            };
            return {
                VideoProcessStatus::Processed,
                std::move(converted)
            };
        } catch (const std::bad_alloc&) {
            av_free(destination_planes[0]);
            return {VideoProcessStatus::Failed, std::nullopt};
        }
    }

private:
    SwsContext* context_{};
};

FfmpegVideoFrameConverter::FfmpegVideoFrameConverter()
    : impl_(std::make_unique<Impl>()) {}

FfmpegVideoFrameConverter::~FfmpegVideoFrameConverter() = default;

VideoProcessResult FfmpegVideoFrameConverter::process(
    const VideoFrame& frame) {
    return impl_->process(frame);
}

} // namespace lapis::media::ffmpeg
