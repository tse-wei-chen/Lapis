#pragma once

#include "audio/audio_frame.hpp"
#include "demux/ffmpeg/ffmpeg_demuxer.hpp"
#include "demux/ffmpeg/ffmpeg_packet_router.hpp"

struct AVCodecContext;
struct AVFrame;

#include <optional>
#include <stop_token>

namespace lapis::media::ffmpeg {

class FfmpegAudioDecoder final {
public:
    FfmpegAudioDecoder() = default;
    ~FfmpegAudioDecoder();

    FfmpegAudioDecoder(const FfmpegAudioDecoder&) = delete;
    FfmpegAudioDecoder& operator=(const FfmpegAudioDecoder&) = delete;

    [[nodiscard]] bool open(const FfmpegAudioStreamInfo& stream_info);
    [[nodiscard]] std::optional<AudioFrame> decode_next(
        FfmpegPacketRouter& packet_router,
        std::stop_token stop_token = {});
    void flush() noexcept;

    [[nodiscard]] int stream_index() const noexcept { return stream_index_; }

private:
    AVCodecContext* context_{};
    AVFrame* frame_{};
    int stream_index_{-1};
    AVRational time_base_{0, 1};
    bool draining_{};
};

} // namespace lapis::media::ffmpeg
