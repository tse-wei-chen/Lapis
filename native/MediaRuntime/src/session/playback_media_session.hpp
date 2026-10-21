#pragma once

#include "audio/audio_frame.hpp"
#include "core/media_time.hpp"
#include "core/subtitle_stream_info.hpp"
#include "core/text_subtitle_cue.hpp"
#include "source/media_source.hpp"
#include "video/video_frame.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace lapis::media {

class GraphPlan;

enum class MediaOpenError : std::uint8_t {
    None,
    UnsupportedSource,
    SourceOpenFailed,
    DemuxFailed,
    DecoderFailed,
    PacketRoutingFailed
};

struct MediaOpenInfo final {
    MediaOpenError error{MediaOpenError::None};
    std::optional<MediaTime> duration;
    bool has_video{};
    std::uint32_t video_width{};
    std::uint32_t video_height{};
    bool has_audio{};
    std::uint32_t audio_sample_rate{};
    std::uint32_t audio_channel_count{};

    [[nodiscard]] bool ok() const noexcept { return error == MediaOpenError::None; }
};

struct MediaDecodeSelection final {
    bool video{};
    bool audio{};
    // Explicit opt-in. Never attach an unconsumed subtitle queue by default.
    std::optional<int> subtitle_stream_index;
};

class PlaybackMediaSession final {
public:
    PlaybackMediaSession();
    ~PlaybackMediaSession();

    PlaybackMediaSession(const PlaybackMediaSession&) = delete;
    PlaybackMediaSession& operator=(const PlaybackMediaSession&) = delete;

    // Composition-time registration: callers must register before Open.
    void register_source_provider(std::unique_ptr<IMediaSourceProvider> provider);
    [[nodiscard]] MediaOpenInfo open(const MediaLocator& locator);
    // Owned metadata snapshot: no raw navigation-controller pointer escapes.
    [[nodiscard]] std::optional<NavigationSnapshot> navigation_snapshot() const;
    [[nodiscard]] bool start_decode_generation(MediaDecodeSelection selection);
    void stop_decode_generation() noexcept;

    // Snapshot is computed once during Open. Safe to copy while decode workers
    // continue reading packets because no FFmpeg-owned memory is exposed.
    [[nodiscard]] std::vector<SubtitleStreamInfo> subtitle_streams() const;

    // Called only by a dedicated subtitle consumer. Returns no cue on
    // cancellation, router drain or unknown/unselected stream.
    [[nodiscard]] std::optional<TextSubtitleCue> decode_next_text_subtitle_cue(
        std::stop_token stop_token = {});

    [[nodiscard]] bool has_video() const noexcept;
    [[nodiscard]] bool has_audio() const noexcept;

    [[nodiscard]] std::optional<VideoFrame> decode_next_video_frame(
        std::stop_token stop_token = {});
    [[nodiscard]] std::optional<AudioFrame> decode_next_audio_frame(
        std::stop_token stop_token = {});

    [[nodiscard]] bool seek(MediaTime target);
    [[nodiscard]] const GraphPlan* graph_plan() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lapis::media
