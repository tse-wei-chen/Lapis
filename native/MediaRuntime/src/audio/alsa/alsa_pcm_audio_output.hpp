#pragma once

#include "audio/audio_output.hpp"
#include "audio/audio_output_provider.hpp"

#include <memory>
#include <string>

namespace lapis::media::alsa {

// PCM device output. The device name is resolved once by the provider; the
// output stream itself belongs to the audio rendering worker, not the UI.
class AlsaPcmAudioOutput final : public IAudioOutput {
public:
    explicit AlsaPcmAudioOutput(std::string device_name);
    ~AlsaPcmAudioOutput() override;
    AlsaPcmAudioOutput(const AlsaPcmAudioOutput&) = delete;
    AlsaPcmAudioOutput& operator=(const AlsaPcmAudioOutput&) = delete;

    [[nodiscard]] AudioOutputOpenStatus open(const AudioFormat& format) override;
    [[nodiscard]] AudioOutputSubmitResult try_submit(
        const AudioFrame& frame, std::uint32_t first_sample_frame) override;
    [[nodiscard]] AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) override;
    [[nodiscard]] AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) override;
    [[nodiscard]] AudioOutputControlStatus pause() override;
    [[nodiscard]] AudioOutputControlStatus resume() override;
    void reset() noexcept override;
    [[nodiscard]] AudioOutputPosition position() const noexcept override;
    void close() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Actual ALSA PCM default device. On distros with pipewire-alsa this resolves
// to PipeWire via ALSA; a native PipeWire sink is still a separate adapter.
class AlsaPcmAudioOutputProvider final : public IAudioOutputProvider {
public:
    [[nodiscard]] AudioOutputPlan prepare_default_output() override;
};

} // namespace lapis::media::alsa
