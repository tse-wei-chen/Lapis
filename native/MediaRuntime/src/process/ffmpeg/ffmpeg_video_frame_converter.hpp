#pragma once

#include "video/video_processor.hpp"

#include <memory>

namespace lapis::media::ffmpeg {

class FfmpegVideoFrameConverter final : public IVideoFrameProcessor {
public:
    FfmpegVideoFrameConverter();
    ~FfmpegVideoFrameConverter() override;

    FfmpegVideoFrameConverter(const FfmpegVideoFrameConverter&) = delete;
    FfmpegVideoFrameConverter& operator=(const FfmpegVideoFrameConverter&) = delete;

    [[nodiscard]] VideoProcessResult process(
        const VideoFrame& frame) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lapis::media::ffmpeg
