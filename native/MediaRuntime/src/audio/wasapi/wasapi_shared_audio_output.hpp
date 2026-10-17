#pragma once

#if defined(_WIN32)

#include "audio/audio_output.hpp"

#include <memory>
#include <stop_token>
#include <string>

namespace lapis::media::wasapi {

class WasapiSharedAudioOutput final : public IAudioOutput {
public:
    WasapiSharedAudioOutput();
    explicit WasapiSharedAudioOutput(std::wstring endpoint_id);
    ~WasapiSharedAudioOutput() override;

    WasapiSharedAudioOutput(const WasapiSharedAudioOutput&) = delete;
    WasapiSharedAudioOutput& operator=(const WasapiSharedAudioOutput&) = delete;

    [[nodiscard]] AudioOutputOpenStatus open(
        const AudioFormat& format) override;
    [[nodiscard]] AudioOutputSubmitResult try_submit(
        const AudioFrame& frame,
        std::uint32_t first_sample_frame) override;
    [[nodiscard]] AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) override;
    [[nodiscard]] AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) override;
    void reset() noexcept override;
    [[nodiscard]] AudioOutputPosition position() const noexcept override;
    void close() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lapis::media::wasapi

#endif
