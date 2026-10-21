#pragma once

#include "core/media_time.hpp"
#include "core/subtitle_stream_info.hpp"
#include "demux/ffmpeg/ffmpeg_packet_router.hpp"
#include "source/media_source.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace lapis::media::ffmpeg {

class FfmpegIoContext;

struct FfmpegVideoStreamInfo final {
    int index{-1};
    int width{};
    int height{};
    AVRational time_base{0, 1};
    MediaTime timestamp_origin{};
    const AVCodecParameters* codec_parameters{};
};

struct FfmpegAudioStreamInfo final {
    int index{-1};
    int sample_rate{};
    int channel_count{};
    AVRational time_base{0, 1};
    MediaTime timestamp_origin{};
    const AVCodecParameters* codec_parameters{};
};

struct FfmpegSubtitleStreamInfo final {
    int index{-1};
    AVRational time_base{0, 1};
    MediaTime timestamp_origin{};
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
    // Enumerate without retaining any FFmpeg-owned objects in the result.
    [[nodiscard]] std::vector<SubtitleStreamInfo> subtitle_streams() const;
    [[nodiscard]] std::optional<FfmpegSubtitleStreamInfo> text_subtitle_stream(
        int stream_index) const noexcept;
    // A standalone probe for native fixtures; does not read/decode packets.
    [[nodiscard]] static std::vector<SubtitleStreamInfo> inspect_subtitle_streams(
        const AVFormatContext& context);
    int read_packet(AVPacket& packet) override;
    [[nodiscard]] bool seek(MediaTime target, int stream_index);

private:
    // One probed packet is owned by the demuxer after a verified Y4M seek.
    // Router consumes it before reading the next FFmpeg packet.
    AVPacket* pending_packet_{};
    bool has_pending_packet_{};
    AVFormatContext* format_{};
    MediaTime timestamp_origin_{};
    std::unique_ptr<FfmpegIoContext> io_;
};

} // namespace lapis::media::ffmpeg
