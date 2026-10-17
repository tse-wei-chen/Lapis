#pragma once

#include "core/media_time.hpp"
#include "core/playback_epoch.hpp"
#include "video/video_frame.hpp"

#include <cstdint>

namespace lapis::media {

enum class VideoPresenterBeginStatus : std::uint8_t {
    Ready,
    Unavailable,
    Failed
};

enum class VideoPresentStatus : std::uint8_t {
    Presented,
    UnsupportedSurface,
    DeviceLost,
    Failed
};

struct VideoPresentContext final {
    PlaybackEpoch epoch;
    MediaTime frame_timestamp{};
    MediaTime scheduled_clock_position{};
    MediaTime scheduled_frame_offset{};
    MediaTime present_clock_position{};
};

class IVideoPresenter {
public:
    virtual ~IVideoPresenter() = default;

    // begin(), present(), and end() are invoked only from the Render Worker
    // thread. Implementations may therefore establish thread-affine graphics
    // state during begin() and release it during end().
    [[nodiscard]] virtual VideoPresenterBeginStatus begin(
        PlaybackEpoch epoch) = 0;

    [[nodiscard]] virtual VideoPresentStatus present(
        const VideoFrame& frame,
        const VideoPresentContext& context) = 0;

    virtual void end() noexcept = 0;
};

} // namespace lapis::media
