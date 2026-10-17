#pragma once

#include "audio/audio_processor.hpp"

#include <optional>

struct SwrContext;

namespace lapis::media::ffmpeg {

class FfmpegAudioFrameConverter final : public IAudioFrameProcessor {
public:
    explicit FfmpegAudioFrameConverter(AudioFormat output_format);
    ~FfmpegAudioFrameConverter() override;

    FfmpegAudioFrameConverter(const FfmpegAudioFrameConverter&) = delete;
    FfmpegAudioFrameConverter& operator=(const FfmpegAudioFrameConverter&) = delete;

    [[nodiscard]] const AudioFormat& output_format() const noexcept override {
        return output_format_;
    }

    [[nodiscard]] AudioProcessResult process(
        const AudioFrame& frame) override;
    [[nodiscard]] AudioProcessResult drain() override;
    void reset() noexcept override;

private:
    AudioFormat output_format_;
    SwrContext* context_{};
    std::optional<AudioFormat> input_format_;
    std::optional<MediaTime> next_output_timestamp_;
    std::uint32_t input_sample_rate_{};
};

} // namespace lapis::media::ffmpeg
