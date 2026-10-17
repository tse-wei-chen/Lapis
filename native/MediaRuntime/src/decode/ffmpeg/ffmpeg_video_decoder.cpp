#include "decode/ffmpeg/ffmpeg_video_decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lapis::media::ffmpeg {
namespace {

VideoPixelFormat to_core_pixel_format(AVPixelFormat format) noexcept {
    switch (format) {
        case AV_PIX_FMT_GRAY8:
            return VideoPixelFormat::Gray8;
        case AV_PIX_FMT_YUV420P:
            return VideoPixelFormat::Yuv420P;
        case AV_PIX_FMT_YUV422P:
            return VideoPixelFormat::Yuv422P;
        case AV_PIX_FMT_YUV444P:
            return VideoPixelFormat::Yuv444P;
        case AV_PIX_FMT_NV12:
            return VideoPixelFormat::Nv12;
        case AV_PIX_FMT_P010LE:
            return VideoPixelFormat::P010;
        case AV_PIX_FMT_BGRA:
            return VideoPixelFormat::Bgra8;
        case AV_PIX_FMT_RGBA:
            return VideoPixelFormat::Rgba8;
        case AV_PIX_FMT_RGB24:
            return VideoPixelFormat::Rgb24;
        default:
            return VideoPixelFormat::Unknown;
    }
}

class FfmpegSystemMemorySurface final : public ISystemMemoryVideoSurface {
public:
    explicit FfmpegSystemMemorySurface(AVFrame* frame)
        : frame_(frame) {}

    ~FfmpegSystemMemorySurface() override {
        av_frame_free(&frame_);
    }

    [[nodiscard]] VideoPixelFormat pixel_format() const noexcept override {
        return to_core_pixel_format(static_cast<AVPixelFormat>(frame_->format));
    }

    [[nodiscard]] std::uint32_t width() const noexcept override {
        return frame_->width > 0 ? static_cast<std::uint32_t>(frame_->width) : 0;
    }

    [[nodiscard]] std::uint32_t height() const noexcept override {
        return frame_->height > 0 ? static_cast<std::uint32_t>(frame_->height) : 0;
    }

    [[nodiscard]] std::uint32_t plane_count() const noexcept override {
        std::uint32_t count{};
        for (std::size_t index = 0;
             index < static_cast<std::size_t>(AV_NUM_DATA_POINTERS);
             ++index) {
            if (frame_->data[index] == nullptr) {
                break;
            }
            ++count;
        }
        return count;
    }

    [[nodiscard]] VideoPlaneView plane(std::uint32_t index) const noexcept override {
        if (index >= static_cast<std::uint32_t>(AV_NUM_DATA_POINTERS) ||
            frame_->data[index] == nullptr) {
            return {};
        }

        return {
            reinterpret_cast<const std::byte*>(frame_->data[index]),
            static_cast<std::ptrdiff_t>(frame_->linesize[index])
        };
    }

private:
    AVFrame* frame_{};
};

std::optional<VideoFrame> make_video_frame(AVFrame& frame, AVRational time_base) {
    auto* clone = av_frame_clone(&frame);
    if (clone == nullptr) {
        return std::nullopt;
    }

    std::shared_ptr<IVideoSurface> surface =
        std::make_shared<FfmpegSystemMemorySurface>(clone);

    VideoFrame result{};
    result.surface = std::move(surface);

    const auto source_pts = frame.best_effort_timestamp;
    if (source_pts != AV_NOPTS_VALUE) {
        result.source_timestamp = SourceTimestamp{
            source_pts,
            Rational{time_base.num, time_base.den}
        };
        result.timestamp = MediaTime{
            av_rescale_q(
                source_pts,
                time_base,
                AVRational{1, 1'000'000'000})
        };
    }

    return result;
}

} // namespace

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    av_frame_free(&frame_);
    avcodec_free_context(&context_);
}

bool FfmpegVideoDecoder::open(const FfmpegVideoStreamInfo& stream_info) {
    if (context_ != nullptr || stream_info.codec_parameters == nullptr || stream_info.index < 0) {
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(stream_info.codec_parameters->codec_id);
    if (codec == nullptr) {
        return false;
    }

    auto* context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        return false;
    }

    if (avcodec_parameters_to_context(context, stream_info.codec_parameters) < 0 ||
        avcodec_open2(context, codec, nullptr) < 0) {
        avcodec_free_context(&context);
        return false;
    }

    auto* frame = av_frame_alloc();
    if (frame == nullptr) {
        avcodec_free_context(&context);
        return false;
    }

    context_ = context;
    frame_ = frame;
    stream_index_ = stream_info.index;
    time_base_ = stream_info.time_base;
    draining_ = false;
    return true;
}

std::optional<VideoFrame> FfmpegVideoDecoder::decode_next(
    FfmpegPacketRouter& packet_router,
    std::stop_token stop_token) {
    if (context_ == nullptr || frame_ == nullptr) {
        return std::nullopt;
    }

    for (;;) {
        if (stop_token.stop_requested()) {
            return std::nullopt;
        }

        const int receive_result = avcodec_receive_frame(context_, frame_);
        if (receive_result == 0) {
            auto result = make_video_frame(*frame_, time_base_);
            av_frame_unref(frame_);
            return result;
        }

        if (receive_result == AVERROR_EOF) {
            return std::nullopt;
        }

        if (receive_result != AVERROR(EAGAIN)) {
            return std::nullopt;
        }

        if (draining_) {
            return std::nullopt;
        }

        auto packet_result = packet_router.read(stream_index_, stop_token);
        if (packet_result.status == FfmpegPacketReadStatus::Stopped) {
            return std::nullopt;
        }
        if (packet_result.status == FfmpegPacketReadStatus::Drained) {
            draining_ = true;
            if (avcodec_send_packet(context_, nullptr) < 0) {
                return std::nullopt;
            }
            continue;
        }
        if (!packet_result.has_packet()) {
            return std::nullopt;
        }

        const int send_result = avcodec_send_packet(
            context_,
            &packet_result.packet->get());
        if (send_result < 0) {
            return std::nullopt;
        }
    }
}

void FfmpegVideoDecoder::flush() noexcept {
    if (context_ != nullptr) {
        avcodec_flush_buffers(context_);
    }
    if (frame_ != nullptr) {
        av_frame_unref(frame_);
    }
    draining_ = false;
}

} // namespace lapis::media::ffmpeg
