#pragma once

#if defined(_WIN32)

#include "audio/audio_output_provider.hpp"

namespace lapis::media::wasapi {

class WasapiSharedAudioOutputProvider final : public IAudioOutputProvider {
public:
    [[nodiscard]] AudioOutputPlan prepare_default_output() override;
};

} // namespace lapis::media::wasapi

#endif
