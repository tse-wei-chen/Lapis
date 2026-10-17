#pragma once

#include "core/media_time.hpp"
#include "source/media_source.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
}

#include <memory>
#include <optional>
#include <string_view>

namespace lapis::media::ffmpeg {

class FfmpegIoContext;

struct FfmpegVideoStreamInfo final {
    int index{-1};
    int width{};
    int height{};
    AVRational time_base{0, 1};
    const AVCodecParameters* codec_parameters{};
};

class FfmpegDemuxer final {
public:
    FfmpegDemuxer();
    ~FfmpegDemuxer();

    FfmpegDemuxer(const FfmpegDemuxer&) = delete;
    FfmpegDemuxer& operator=(const FfmpegDemuxer&) = delete;

    [[nodiscard]] bool open(IMediaByteStream& stream, std::string_view demux_hint);
    [[nodiscard]] std::optional<MediaTime> duration() const noexcept;
    [[nodiscard]] std::optional<FfmpegVideoStreamInfo> best_video_stream() const noexcept;
    int read_packet(AVPacket& packet);
    [[nodiscard]] bool seek(MediaTime target, int stream_index);

private:
    AVFormatContext* format_{};
    std::unique_ptr<FfmpegIoContext> io_;
};

} // namespace lapis::media::ffmpeg
