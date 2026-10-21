#pragma once

#include "core/text_subtitle_cue.hpp"
#include "demux/ffmpeg/ffmpeg_demuxer.hpp"
#include "demux/ffmpeg/ffmpeg_packet_router.hpp"

struct AVCodecContext;

#include <optional>
#include <stop_token>

namespace lapis::media::ffmpeg {

// Opt-in, bounded text-only decoder; never decodes/render PGS or DVD bitmaps.
// This consumer must drain its selected packet queue while the router runs.
class FfmpegTextSubtitleDecoder final {
public:
    FfmpegTextSubtitleDecoder() = default;
    ~FfmpegTextSubtitleDecoder();

    FfmpegTextSubtitleDecoder(const FfmpegTextSubtitleDecoder&) = delete;
    FfmpegTextSubtitleDecoder& operator=(const FfmpegTextSubtitleDecoder&) = delete;

    [[nodiscard]] bool open(const FfmpegSubtitleStreamInfo& stream);
    [[nodiscard]] std::optional<TextSubtitleCue> decode_next(
        FfmpegPacketRouter& router, std::stop_token stop = {});
    void flush() noexcept;

    [[nodiscard]] int stream_index() const noexcept { return stream_index_; }

private:
    AVCodecContext* codec_{};
    int stream_index_{-1};
    AVRational time_base_{0, 1};
    MediaTime timestamp_origin_{};
};
} // namespace lapis::media::ffmpeg
