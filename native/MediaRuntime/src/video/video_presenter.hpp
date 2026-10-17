#pragma once

#include "core/media_time.hpp"
#include "core/playback_epoch.hpp"
#include "video/video_frame.hpp"
#include "video/video_output_surface.hpp"

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

    // Output-surface changes are invoked only from the Render Worker thread. A
    // Ready result is an acknowledgement boundary: after it returns, the
    // presenter must no longer access resources owned by the previously bound
    // output surface. The default keeps headless presenters source-compatible.
    [[nodiscard]] virtual VideoOutputSurfaceApplyStatus set_output_surface(
        const VideoOutputSurfaceDescriptor& surface) {
        return surface.kind == VideoOutputSurfaceKind::None
            ? VideoOutputSurfaceApplyStatus::Ready
            : VideoOutputSurfaceApplyStatus::Unsupported;
    }

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
