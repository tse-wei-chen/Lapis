#pragma once

#include "video/video_frame.hpp"

#include <cstdint>
#include <optional>

namespace lapis::media {

enum class VideoProcessStatus : std::uint8_t {
    Processed,
    UnsupportedInput,
    Failed
};

struct VideoProcessResult final {
    VideoProcessStatus status{VideoProcessStatus::Failed};
    std::optional<VideoFrame> frame;

    [[nodiscard]] bool ok() const noexcept {
        return status == VideoProcessStatus::Processed && frame.has_value();
    }
};

class IVideoFrameProcessor {
public:
    virtual ~IVideoFrameProcessor() = default;

    [[nodiscard]] virtual VideoProcessResult process(
        const VideoFrame& frame) = 0;
};

} // namespace lapis::media
