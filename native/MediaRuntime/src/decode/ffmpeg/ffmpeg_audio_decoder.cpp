#include "decode/ffmpeg/ffmpeg_audio_decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace lapis::media::ffmpeg {
namespace {

std::int64_t subtract_timestamp_origin(
    std::int64_t timestamp,
    std::int64_t origin) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (origin > 0 && timestamp < minimum + origin) {
        return minimum;
    }
    if (origin < 0 && timestamp > maximum + origin) {
        return maximum;
    }
    return timestamp - origin;
}

AudioSampleFormat to_core_sample_format(AVSampleFormat format) noexcept {
    const auto packed = av_get_packed_sample_fmt(format);
    switch (packed) {
        case AV_SAMPLE_FMT_U8:
            return AudioSampleFormat::U8;
        case AV_SAMPLE_FMT_S16:
            return AudioSampleFormat::S16;
        case AV_SAMPLE_FMT_S32:
            return AudioSampleFormat::S32;
        case AV_SAMPLE_FMT_S64:
            return AudioSampleFormat::S64;
        case AV_SAMPLE_FMT_FLT:
            return AudioSampleFormat::F32;
        case AV_SAMPLE_FMT_DBL:
            return AudioSampleFormat::F64;
        default:
            return AudioSampleFormat::Unknown;
    }
}

AudioChannel to_core_channel(AVChannel channel) noexcept {
    switch (channel) {
        case AV_CHAN_FRONT_LEFT:
            return AudioChannel::FrontLeft;
        case AV_CHAN_FRONT_RIGHT:
            return AudioChannel::FrontRight;
        case AV_CHAN_FRONT_CENTER:
            return AudioChannel::FrontCenter;
        case AV_CHAN_LOW_FREQUENCY:
            return AudioChannel::LowFrequency;
        case AV_CHAN_BACK_LEFT:
            return AudioChannel::BackLeft;
        case AV_CHAN_BACK_RIGHT:
            return AudioChannel::BackRight;
        case AV_CHAN_FRONT_LEFT_OF_CENTER:
            return AudioChannel::FrontLeftOfCenter;
        case AV_CHAN_FRONT_RIGHT_OF_CENTER:
            return AudioChannel::FrontRightOfCenter;
        case AV_CHAN_BACK_CENTER:
            return AudioChannel::BackCenter;
        case AV_CHAN_SIDE_LEFT:
            return AudioChannel::SideLeft;
        case AV_CHAN_SIDE_RIGHT:
            return AudioChannel::SideRight;
        case AV_CHAN_TOP_CENTER:
            return AudioChannel::TopCenter;
        case AV_CHAN_TOP_FRONT_LEFT:
            return AudioChannel::TopFrontLeft;
        case AV_CHAN_TOP_FRONT_CENTER:
            return AudioChannel::TopFrontCenter;
        case AV_CHAN_TOP_FRONT_RIGHT:
            return AudioChannel::TopFrontRight;
        case AV_CHAN_TOP_BACK_LEFT:
            return AudioChannel::TopBackLeft;
        case AV_CHAN_TOP_BACK_CENTER:
            return AudioChannel::TopBackCenter;
        case AV_CHAN_TOP_BACK_RIGHT:
            return AudioChannel::TopBackRight;
        default:
            return AudioChannel::Unknown;
    }
}

AudioFormat make_audio_format(const AVFrame& frame) {
    const auto sample_format = static_cast<AVSampleFormat>(frame.format);
    AudioFormat result{};
    result.sample_format = to_core_sample_format(sample_format);
    result.packing = av_sample_fmt_is_planar(sample_format) != 0
        ? AudioPacking::Planar
        : AudioPacking::Interleaved;
    result.sample_rate = frame.sample_rate > 0
        ? static_cast<std::uint32_t>(frame.sample_rate)
        : 0;

    AVChannelLayout inferred_layout{};
    const AVChannelLayout* layout = &frame.ch_layout;
    if (frame.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC &&
        frame.ch_layout.nb_channels > 0) {
        av_channel_layout_default(
            &inferred_layout,
            frame.ch_layout.nb_channels);
        if (inferred_layout.nb_channels == frame.ch_layout.nb_channels) {
            layout = &inferred_layout;
            result.channel_layout_inferred = true;
        }
    }

    if (layout->nb_channels > 0) {
        const auto channel_count = static_cast<unsigned int>(layout->nb_channels);
        result.channels.reserve(static_cast<std::size_t>(channel_count));
        for (unsigned int index = 0; index < channel_count; ++index) {
            result.channels.push_back(to_core_channel(
                av_channel_layout_channel_from_index(layout, index)));
        }
    }

    av_channel_layout_uninit(&inferred_layout);
    return result;
}

class FfmpegAudioBuffer final : public IAudioBuffer {
public:
    FfmpegAudioBuffer(
        AVFrame* frame,
        AudioFormat format,
        std::uint32_t bytes_per_sample) noexcept
        : frame_(frame),
          format_(std::move(format)),
          bytes_per_sample_(bytes_per_sample) {}

    ~FfmpegAudioBuffer() override {
        av_frame_free(&frame_);
    }

    [[nodiscard]] const AudioFormat& format() const noexcept override {
        return format_;
    }

    [[nodiscard]] std::uint32_t sample_frame_count() const noexcept override {
        return frame_ != nullptr && frame_->nb_samples > 0
            ? static_cast<std::uint32_t>(frame_->nb_samples)
            : 0;
    }

    [[nodiscard]] std::uint32_t bytes_per_sample() const noexcept override {
        return bytes_per_sample_;
    }

    [[nodiscard]] std::uint32_t plane_count() const noexcept override {
        if (frame_ == nullptr || frame_->extended_data == nullptr ||
            format_.channels.empty()) {
            return 0;
        }
        return format_.packing == AudioPacking::Planar
            ? static_cast<std::uint32_t>(format_.channels.size())
            : 1U;
    }

    [[nodiscard]] AudioPlaneView plane(std::uint32_t index) const noexcept override {
        const auto planes = plane_count();
        if (index >= planes || frame_ == nullptr || frame_->extended_data == nullptr ||
            frame_->extended_data[index] == nullptr || bytes_per_sample_ == 0) {
            return {};
        }

        const auto channel_factor = format_.packing == AudioPacking::Planar
            ? std::size_t{1}
            : format_.channels.size();
        const auto frames = static_cast<std::size_t>(sample_frame_count());
        const auto sample_bytes = static_cast<std::size_t>(bytes_per_sample_);
        if (frames > std::numeric_limits<std::size_t>::max() / sample_bytes ||
            frames * sample_bytes >
                std::numeric_limits<std::size_t>::max() / channel_factor) {
            return {};
        }

        return {
            reinterpret_cast<const std::byte*>(frame_->extended_data[index]),
            frames * sample_bytes * channel_factor
        };
    }

private:
    AVFrame* frame_{};
    AudioFormat format_;
    std::uint32_t bytes_per_sample_{};
};

std::optional<AudioFrame> make_audio_frame(
    AVFrame& frame,
    AVRational time_base,
    MediaTime timestamp_origin) {
    const auto sample_format = static_cast<AVSampleFormat>(frame.format);
    const int sample_bytes = av_get_bytes_per_sample(sample_format);
    if (sample_bytes <= 0 || frame.nb_samples <= 0 || frame.sample_rate <= 0) {
        return std::nullopt;
    }

    auto format = make_audio_format(frame);
    if (format.sample_format == AudioSampleFormat::Unknown ||
        format.channels.empty()) {
        return std::nullopt;
    }

    auto* clone = av_frame_clone(&frame);
    if (clone == nullptr) {
        return std::nullopt;
    }

    try {
        auto buffer = std::make_shared<FfmpegAudioBuffer>(
            clone,
            std::move(format),
            static_cast<std::uint32_t>(sample_bytes));
        clone = nullptr;

        AudioFrame result{};
        result.buffer = std::move(buffer);
        result.duration = MediaTime{
            av_rescale_q(
                frame.nb_samples,
                AVRational{1, frame.sample_rate},
                AVRational{1, 1'000'000'000})
        };

        const auto source_pts = frame.best_effort_timestamp;
        if (source_pts != AV_NOPTS_VALUE) {
            result.source_timestamp = SourceTimestamp{
                source_pts,
                Rational{time_base.num, time_base.den}
            };
            const auto absolute_timestamp = av_rescale_q(
                    source_pts,
                    time_base,
                    AVRational{1, 1'000'000'000});
            result.timestamp = MediaTime{subtract_timestamp_origin(
                absolute_timestamp,
                timestamp_origin.nanoseconds)};
        }

        return result;
    } catch (...) {
        av_frame_free(&clone);
        throw;
    }
}

} // namespace

FfmpegAudioDecoder::~FfmpegAudioDecoder() {
    av_frame_free(&frame_);
    avcodec_free_context(&context_);
}

bool FfmpegAudioDecoder::open(const FfmpegAudioStreamInfo& stream_info) {
    if (context_ != nullptr || stream_info.codec_parameters == nullptr ||
        stream_info.index < 0) {
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
    timestamp_origin_ = stream_info.timestamp_origin;
    draining_ = false;
    return true;
}

std::optional<AudioFrame> FfmpegAudioDecoder::decode_next(
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
            auto result = make_audio_frame(
                *frame_, time_base_, timestamp_origin_);
            av_frame_unref(frame_);
            return result;
        }
        if (receive_result == AVERROR_EOF) {
            return std::nullopt;
        }
        if (receive_result != AVERROR(EAGAIN) || draining_) {
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

        if (avcodec_send_packet(context_, &packet_result.packet->get()) < 0) {
            return std::nullopt;
        }
    }
}

void FfmpegAudioDecoder::flush() noexcept {
    if (context_ != nullptr) {
        avcodec_flush_buffers(context_);
    }
    if (frame_ != nullptr) {
        av_frame_unref(frame_);
    }
    draining_ = false;
}

} // namespace lapis::media::ffmpeg
