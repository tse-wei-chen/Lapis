#include "demux/ffmpeg/ffmpeg_demuxer.hpp"

#include "demux/ffmpeg/ffmpeg_io.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavcodec/codec_desc.h>
#include <libavutil/dict.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstdint>
#include <utility>
#include <cstring>
#include <limits>
#include <string>

namespace lapis::media::ffmpeg {
namespace {

bool checked_add(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& result) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((right > 0 && left > maximum - right) ||
        (right < 0 && left < minimum - right)) {
        return false;
    }
    result = left + right;
    return true;
}

} // namespace

FfmpegDemuxer::FfmpegDemuxer() = default;

void FfmpegDemuxer::request_interrupt() noexcept {
    read_interrupt_requested_.store(true, std::memory_order_release);
}

void FfmpegDemuxer::reset_interrupt() noexcept {
    read_interrupt_requested_.store(false, std::memory_order_release);
}

int FfmpegDemuxer::interrupt_callback(void* opaque) noexcept {
    if (opaque == nullptr) return 0;
    const auto* self = static_cast<const FfmpegDemuxer*>(opaque);
    return self->read_interrupt_requested_.load(std::memory_order_acquire) ? 1 : 0;
}

FfmpegDemuxer::~FfmpegDemuxer() {
    av_packet_free(&pending_packet_);
    if (format_ != nullptr) {
        avformat_close_input(&format_);
    }
    io_.reset();
}

bool FfmpegDemuxer::open(IMediaByteStream& stream, std::string_view demux_hint) {
    if (format_ != nullptr) {
        return false;
    }

    auto io = std::make_unique<FfmpegIoContext>(stream, read_interrupt_requested_);
    if (!io->valid()) {
        return false;
    }

    auto* context = avformat_alloc_context();
    if (context == nullptr) {
        return false;
    }

    context->pb = io->get();
    context->flags |= AVFMT_FLAG_CUSTOM_IO;
    context->interrupt_callback.callback = &FfmpegDemuxer::interrupt_callback;
    context->interrupt_callback.opaque = this;

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
    if (context->start_time != AV_NOPTS_VALUE) {
        timestamp_origin_ = MediaTime{
            av_rescale_q(
                context->start_time,
                AV_TIME_BASE_Q,
                AVRational{1, 1'000'000'000})
        };
    }
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
        timestamp_origin_,
        stream->codecpar
    };
}

std::optional<FfmpegAudioStreamInfo> FfmpegDemuxer::best_audio_stream() const noexcept {
    if (format_ == nullptr) {
        return std::nullopt;
    }

    const int index = av_find_best_stream(
        format_,
        AVMEDIA_TYPE_AUDIO,
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

    return FfmpegAudioStreamInfo{
        index,
        stream->codecpar->sample_rate,
        stream->codecpar->ch_layout.nb_channels,
        stream->time_base,
        timestamp_origin_,
        stream->codecpar
    };
}

std::vector<SubtitleStreamInfo> FfmpegDemuxer::inspect_subtitle_streams(
    const AVFormatContext& context) {
    // FFmpeg exposes a subtitle stream even when its actual subtitle decoder
    // is unavailable. Listing is NOT a claim that we can render the track.
    constexpr unsigned int max_tracks = 64;
    std::vector<SubtitleStreamInfo> tracks;
    tracks.reserve(std::min(context.nb_streams, max_tracks));
    if (context.streams == nullptr) return tracks;

    for (unsigned int i = 0; i < context.nb_streams && tracks.size() < max_tracks; ++i) {
        const auto* stream = context.streams[i];
        if (stream == nullptr || stream->codecpar == nullptr ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

        const auto codec = stream->codecpar->codec_id;
        const auto* descriptor = avcodec_descriptor_get(codec);
        SubtitleTrackKind kind = SubtitleTrackKind::Unknown;
        if (descriptor != nullptr && (descriptor->props & AV_CODEC_PROP_TEXT_SUB) != 0)
            kind = SubtitleTrackKind::Text;
        else if (descriptor != nullptr && (descriptor->props & AV_CODEC_PROP_BITMAP_SUB) != 0)
            kind = SubtitleTrackKind::Bitmap;

        const auto* language = av_dict_get(stream->metadata, "language", nullptr, 0);
        // ISO 639 / BCP47 tags fit the public ABI. Bound allocations even
        // when container metadata is untrusted or malformed.
        std::string language_tag;
        if (language != nullptr && language->value != nullptr) {
            for (std::size_t j = 0; j < 23 && language->value[j] != '\0'; ++j)
                language_tag.push_back(language->value[j]);
        }
        tracks.push_back(SubtitleStreamInfo{
            static_cast<std::uint32_t>(i),
            kind,
            std::move(language_tag),
            std::string(avcodec_get_name(codec))
        });
    }
    return tracks;
}

std::vector<SubtitleStreamInfo> FfmpegDemuxer::subtitle_streams() const {
    return format_ != nullptr
        ? inspect_subtitle_streams(*format_)
        : std::vector<SubtitleStreamInfo>{};
}

std::optional<FfmpegSubtitleStreamInfo> FfmpegDemuxer::text_subtitle_stream(
    int stream_index) const noexcept {
    if (format_ == nullptr || stream_index < 0 ||
        static_cast<unsigned>(stream_index) >= format_->nb_streams)
        return std::nullopt;

    const auto* stream = format_->streams[stream_index];
    if (stream == nullptr || stream->codecpar == nullptr ||
        stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
        return std::nullopt;

    // Allow only codecs that the first text decoder can actually decode.
    if (stream->codecpar->codec_id != AV_CODEC_ID_SUBRIP &&
        stream->codecpar->codec_id != AV_CODEC_ID_MOV_TEXT)
        return std::nullopt;
    return FfmpegSubtitleStreamInfo{
        stream_index, stream->time_base, timestamp_origin_, stream->codecpar
    };
}

int FfmpegDemuxer::read_packet(AVPacket& packet) {
    if (read_interrupt_requested_.load(std::memory_order_acquire)) {
        return AVERROR_EXIT;
    }
    if (format_ == nullptr) {
        return AVERROR(EINVAL);
    }
    if (has_pending_packet_) {
        av_packet_move_ref(&packet, pending_packet_);
        has_pending_packet_ = false;
        return 0;
    }
    return av_read_frame(format_, &packet);
}

bool FfmpegDemuxer::seek(MediaTime target, int stream_index) {
    if (format_ == nullptr || target.nanoseconds < 0) {
        return false;
    }
    // Only called after the previous packet router has been joined.
    reset_interrupt();

    std::int64_t absolute_target_ns{};
    if (!checked_add(
            target.nanoseconds,
            timestamp_origin_.nanoseconds,
            absolute_target_ns)) {
        return false;
    }

    // Drop an unconsumed look-ahead from any previous generation.
    if (pending_packet_ != nullptr) {
        av_packet_unref(pending_packet_);
    }
    has_pending_packet_ = false;

    std::int64_t timestamp{};
    if (stream_index >= 0 &&
        static_cast<unsigned int>(stream_index) < format_->nb_streams) {
        const auto* stream = format_->streams[stream_index];
        timestamp = av_rescale_q(
            absolute_target_ns,
            AVRational{1, 1'000'000'000},
            stream->time_base);
    } else {
        timestamp = av_rescale_q(
            absolute_target_ns,
            AVRational{1, 1'000'000'000},
            AV_TIME_BASE_Q);
        stream_index = -1;
    }

    int status = av_seek_frame(
        format_, stream_index, timestamp, AVSEEK_FLAG_BACKWARD);
    const auto* input_format = format_->iformat;
    const bool is_y4m = input_format != nullptr &&
        input_format->name != nullptr &&
        std::strcmp(input_format->name, "yuv4mpegpipe") == 0;

    // Unindexed transport streams (including clear BDMV) may reject a
    // stream-specific seek while accepting a demuxer-global timestamp seek.
    if (status < 0 && !is_y4m) {
        const auto global_timestamp = av_rescale_q(
            absolute_target_ns, AVRational{1, 1'000'000'000}, AV_TIME_BASE_Q);
        status = avformat_seek_file(
            format_, -1, std::numeric_limits<std::int64_t>::min(),
            global_timestamp, std::numeric_limits<std::int64_t>::max(),
            AVSEEK_FLAG_BACKWARD);
    }

    // Ordinary indexed containers preserve FFmpeg's native seek behavior.
    if (!is_y4m || target.nanoseconds == 0 || stream_index < 0) {
        return status >= 0;
    }

    // Small Y4M sources can be entirely prefetched through custom AVIO. Some
    // FFmpeg builds return a successful timestamp seek but immediately report
    // EOF on the next packet. A return code alone is therefore insufficient.
    // Probe exactly one packet *before* returning to the decoder, and retain
    // it so no valid frame is silently skipped.
    if (pending_packet_ == nullptr) {
        pending_packet_ = av_packet_alloc();
        if (pending_packet_ == nullptr) {
            return false;
        }
    }
    if (status >= 0 &&
        av_read_frame(format_, pending_packet_) >= 0) {
        has_pending_packet_ = true;
        return true;
    }
    av_packet_unref(pending_packet_);

    // Y4M is intra-frame and has monotonically increasing frame timestamps.
    // Recovery can therefore restart at zero and scan to the requested
    // timestamp. The hard cap prevents an unindexed multi-hour stream from
    // causing unbounded latency on the calling control thread. Never publish
    // success until an actual packet at/after the target is retained.
    if (av_seek_frame(format_, stream_index, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    constexpr int max_fallback_packets = 5000;
    constexpr std::uint64_t max_fallback_bytes = 256ull * 1024 * 1024;
    std::uint64_t scanned_bytes{};
    for (int count = 0; count < max_fallback_packets; ++count) {
        if (av_read_frame(format_, pending_packet_) < 0) {
            av_packet_unref(pending_packet_);
            return false;
        }
        // A frame cap alone is insufficient for high-resolution raw Y4M:
        // 5000 4K frames would still read many gigabytes synchronously.
        const auto packet_bytes = pending_packet_->size;
        if (packet_bytes < 0 ||
            static_cast<std::uint64_t>(packet_bytes) >
                max_fallback_bytes - scanned_bytes) {
            av_packet_unref(pending_packet_);
            return false;
        }
        scanned_bytes += static_cast<std::uint64_t>(packet_bytes);
        if (pending_packet_->stream_index == stream_index &&
            pending_packet_->pts != AV_NOPTS_VALUE &&
            pending_packet_->pts >= timestamp) {
            has_pending_packet_ = true;
            return true;
        }
        av_packet_unref(pending_packet_);
    }
    return false;
}

} // namespace lapis::media::ffmpeg
