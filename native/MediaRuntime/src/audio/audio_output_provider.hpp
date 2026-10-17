#pragma once

#include "audio/audio_frame.hpp"
#include "audio/audio_output.hpp"

#include <cstdint>
#include <memory>

namespace lapis::media {

// Preparing an output is intentionally separate from opening the device stream.
// The provider selects a stable endpoint and exposes the exact Lapis-owned PCM
// format that upstream processing should target before AudioRenderWorker starts.
enum class AudioOutputPrepareStatus : std::uint8_t {
    Ready,
    UnsupportedFormat,
    DeviceUnavailable,
    Faulted
};

struct AudioOutputPlan final {
    AudioOutputPrepareStatus status{AudioOutputPrepareStatus::Faulted};
    AudioFormat target_format{};
    std::unique_ptr<IAudioOutput> output;

    [[nodiscard]] bool ok() const noexcept {
        return status == AudioOutputPrepareStatus::Ready &&
            output != nullptr &&
            target_format.sample_format != AudioSampleFormat::Unknown &&
            target_format.sample_rate != 0 &&
            !target_format.channels.empty();
    }
};

class IAudioOutputProvider {
public:
    virtual ~IAudioOutputProvider() = default;

    // Returns a stream object bound to the same endpoint used to discover the
    // target format. Empty/default-device policy remains in the provider; the
    // render worker only receives an exact format and an IAudioOutput instance.
    [[nodiscard]] virtual AudioOutputPlan prepare_default_output() = 0;
};

} // namespace lapis::media
