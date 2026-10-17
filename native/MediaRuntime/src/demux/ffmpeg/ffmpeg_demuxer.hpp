#pragma once

#include "core/media_time.hpp"
#include "demux/ffmpeg/ffmpeg_packet_router.hpp"
#include "source/media_source.hpp"

extern "C" {
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

struct FfmpegAudioStreamInfo final {
    int index{-1};
    int sample_rate{};
    int channel_count{};
    AVRational time_base{0, 1};
    const AVCodecParameters* codec_parameters{};
};

class FfmpegDemuxer final : public IFfmpegPacketSource {
public:
    FfmpegDemuxer();
    ~FfmpegDemuxer() override;

    FfmpegDemuxer(const FfmpegDemuxer&) = delete;
    FfmpegDemuxer& operator=(const FfmpegDemuxer&) = delete;

    [[nodiscard]] bool open(IMediaByteStream& stream, std::string_view demux_hint);
    [[nodiscard]] std::optional<MediaTime> duration() const noexcept;
    [[nodiscard]] std::optional<FfmpegVideoStreamInfo> best_video_stream() const noexcept;
    [[nodiscard]] std::optional<FfmpegAudioStreamInfo> best_audio_stream() const noexcept;
    int read_packet(AVPacket& packet) override;
    [[nodiscard]] bool seek(MediaTime target, int stream_index);

private:
    AVFormatContext* format_{};
    std::unique_ptr<FfmpegIoContext> io_;
};

} // namespace lapis::media::ffmpeg
