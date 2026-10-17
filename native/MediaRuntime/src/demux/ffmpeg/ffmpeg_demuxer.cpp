#include "demux/ffmpeg/ffmpeg_demuxer.hpp"

#include "demux/ffmpeg/ffmpeg_io.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <string>

namespace lapis::media::ffmpeg {

FfmpegDemuxer::~FfmpegDemuxer() {
    if (format_ != nullptr) {
        avformat_close_input(&format_);
    }
    io_.reset();
}

bool FfmpegDemuxer::open(IMediaByteStream& stream, std::string_view demux_hint) {
    if (format_ != nullptr) {
        return false;
    }

    auto io = std::make_unique<FfmpegIoContext>(stream);
    if (!io->valid()) {
        return false;
    }

    auto* context = avformat_alloc_context();
    if (context == nullptr) {
        return false;
    }

    context->pb = io->get();
    context->flags |= AVFMT_FLAG_CUSTOM_IO;

    const std::string hint(demux_hint);
    const char* url_hint = hint.empty() ? nullptr : hint.c_str();
    if (avformat_open_input(&context, url_hint, nullptr, nullptr) < 0) {
        if (context != nullptr) {
            avformat_free_context(context);
        }
        return false;
    }

    if (avformat_find_stream_info(context, nullptr) < 0) {
        avformat_close_input(&context);
        return false;
    }

    format_ = context;
    io_ = std::move(io);
    return true;
}

std::optional<MediaTime> FfmpegDemuxer::duration() const noexcept {
    if (format_ == nullptr || format_->duration == AV_NOPTS_VALUE || format_->duration < 0) {
        return std::nullopt;
    }

    return MediaTime{
        av_rescale_q(
            format_->duration,
            AV_TIME_BASE_Q,
            AVRational{1, 1'000'000'000})
    };
}

std::optional<FfmpegVideoStreamInfo> FfmpegDemuxer::best_video_stream() const noexcept {
    if (format_ == nullptr) {
        return std::nullopt;
    }

    const int index = av_find_best_stream(
        format_,
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0);
    if (index < 0) {
        return std::nullopt;
    }

    const auto* stream = format_->streams[index];
    if (stream == nullptr || stream->codecpar == nullptr) {
        return std::nullopt;
    }

    return FfmpegVideoStreamInfo{
        index,
        stream->codecpar->width,
        stream->codecpar->height,
        stream->time_base,
        stream->codecpar
    };
}

int FfmpegDemuxer::read_packet(AVPacket& packet) {
    if (format_ == nullptr) {
        return AVERROR(EINVAL);
    }
    return av_read_frame(format_, &packet);
}

bool FfmpegDemuxer::seek(MediaTime target, int stream_index) {
    if (format_ == nullptr || target.nanoseconds < 0) {
        return false;
    }

    std::int64_t timestamp{};
    if (stream_index >= 0 && static_cast<unsigned int>(stream_index) < format_->nb_streams) {
        const auto* stream = format_->streams[stream_index];
        timestamp = av_rescale_q(
            target.nanoseconds,
            AVRational{1, 1'000'000'000},
            stream->time_base);
    } else {
        timestamp = av_rescale_q(
            target.nanoseconds,
            AVRational{1, 1'000'000'000},
            AV_TIME_BASE_Q);
        stream_index = -1;
    }

    if (av_seek_frame(format_, stream_index, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    avformat_flush(format_);
    return true;
}

} // namespace lapis::media::ffmpeg
