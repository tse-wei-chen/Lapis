#pragma once

#include "audio/audio_frame.hpp"

#include <cstdint>
#include <optional>

namespace lapis::media {

enum class AudioProcessStatus : std::uint8_t {
    Processed,
    NeedMoreInput,
    Drained,
    UnsupportedInput,
    Failed
};

struct AudioProcessResult final {
    AudioProcessStatus status{AudioProcessStatus::Failed};
    std::optional<AudioFrame> frame;

    [[nodiscard]] bool has_frame() const noexcept {
        return status == AudioProcessStatus::Processed && frame.has_value();
    }
};

class IAudioFrameProcessor {
public:
    virtual ~IAudioFrameProcessor() = default;

    [[nodiscard]] virtual const AudioFormat& output_format() const noexcept = 0;
    [[nodiscard]] virtual AudioProcessResult process(
        const AudioFrame& frame) = 0;

    // Audio processors may buffer samples (especially while resampling).
    // Drain is called repeatedly after input EOF until Drained is returned.
    [[nodiscard]] virtual AudioProcessResult drain() = 0;

    // Discards buffered samples at epoch discontinuities.
    virtual void reset() noexcept = 0;
};

} // namespace lapis::media
