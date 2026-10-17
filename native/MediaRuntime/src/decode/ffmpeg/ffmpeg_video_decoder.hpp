#pragma once

#include "demux/ffmpeg/ffmpeg_demuxer.hpp"
#include "video/video_frame.hpp"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

#include <optional>

namespace lapis::media::ffmpeg {

class FfmpegVideoDecoder final {
public:
    FfmpegVideoDecoder() = default;
    ~FfmpegVideoDecoder();

    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    [[nodiscard]] bool open(const FfmpegVideoStreamInfo& stream_info);
    [[nodiscard]] std::optional<VideoFrame> decode_next(FfmpegDemuxer& demuxer);
    void flush() noexcept;

    [[nodiscard]] int stream_index() const noexcept { return stream_index_; }

private:
    AVCodecContext* context_{};
    AVFrame* frame_{};
    AVPacket* packet_{};
    int stream_index_{-1};
    AVRational time_base_{0, 1};
    bool draining_{};
};

} // namespace lapis::media::ffmpeg
