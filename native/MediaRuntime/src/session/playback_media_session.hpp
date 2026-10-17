#pragma once

#include "core/media_time.hpp"
#include "source/media_source.hpp"
#include "video/video_frame.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

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

    [[nodiscard]] bool ok() const noexcept { return error == MediaOpenError::None; }
};

class PlaybackMediaSession final {
public:
    PlaybackMediaSession();
    ~PlaybackMediaSession();

    PlaybackMediaSession(const PlaybackMediaSession&) = delete;
    PlaybackMediaSession& operator=(const PlaybackMediaSession&) = delete;

    [[nodiscard]] MediaOpenInfo open(const MediaLocator& locator);
    [[nodiscard]] std::optional<VideoFrame> decode_next_video_frame(
        std::stop_token stop_token = {});
    [[nodiscard]] bool seek(MediaTime target);
    [[nodiscard]] const GraphPlan* graph_plan() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lapis::media
