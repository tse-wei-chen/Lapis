#include "audio/alsa/alsa_pcm_audio_output.hpp"

#include "audio/audio_output.hpp"
#include "audio/audio_output_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stop_token>

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "CHECK failed: " #expression << " at line " \
                  << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {

class TestBuffer final : public lapis::media::IAudioBuffer {
public:
    TestBuffer() {
        format_.sample_format = lapis::media::AudioSampleFormat::F32;
        format_.packing = lapis::media::AudioPacking::Interleaved;
        format_.sample_rate = 48'000;
        format_.channels = {
            lapis::media::AudioChannel::FrontLeft,
            lapis::media::AudioChannel::FrontRight
        };
    }

    [[nodiscard]] const lapis::media::AudioFormat& format() const noexcept override {
        return format_;
    }
    [[nodiscard]] std::uint32_t sample_frame_count() const noexcept override {
        return 480;
    }
    [[nodiscard]] std::uint32_t bytes_per_sample() const noexcept override {
        return sizeof(float);
    }
    [[nodiscard]] std::uint32_t plane_count() const noexcept override { return 1; }
    [[nodiscard]] lapis::media::AudioPlaneView plane(
        std::uint32_t index) const noexcept override {
        if (index != 0) return {};
        return {
            reinterpret_cast<const std::byte*>(samples_.data()),
            samples_.size() * sizeof(float)
        };
    }

private:
    lapis::media::AudioFormat format_;
    std::array<float, 960> samples_{};
};

} // namespace

int main() {
    using namespace lapis::media;
    using namespace lapis::media::alsa;

    // Linux CI selects ALSA's actual 'null' PCM plugin. The C++ adapter
    // still performs device open/configure/write/poll/drain against libasound.
    AlsaPcmAudioOutputProvider provider;
    auto plan = provider.prepare_default_output();
    CHECK(plan.ok());
    CHECK(plan.target_format.sample_rate == 48'000);
    CHECK(plan.target_format.sample_format == AudioSampleFormat::F32);
    CHECK(plan.target_format.packing == AudioPacking::Interleaved);
    CHECK(plan.target_format.channels.size() == 2);
    auto output = std::move(plan.output);

    auto unsupported = plan.target_format;
    unsupported.sample_rate = 44'100;
    CHECK(output->open(unsupported) == AudioOutputOpenStatus::UnsupportedFormat);
    CHECK(output->open(plan.target_format) == AudioOutputOpenStatus::Ready);

    AudioFrame frame;
    frame.buffer = std::make_shared<TestBuffer>();
    CHECK(output->try_submit(frame, 480).status == AudioOutputStatus::Faulted);
    std::uint32_t submitted{};
    std::stop_source stop;
    for (unsigned attempts = 0; submitted < 480 && attempts < 100; ++attempts) {
        const auto sent = output->try_submit(frame, submitted);
        if (sent.status == AudioOutputStatus::Backpressure) {
            CHECK(output->wait_until_writable(stop.get_token()) ==
                AudioOutputWaitStatus::Ready);
            continue;
        }
        CHECK(sent.status == AudioOutputStatus::Accepted);
        CHECK(sent.accepted_sample_frames > 0);
        CHECK(sent.accepted_sample_frames <= 480 - submitted);
        submitted += sent.accepted_sample_frames;
    }
    CHECK(submitted == 480);
    CHECK(output->wait_until_drained(stop.get_token()) ==
        AudioOutputWaitStatus::Ready);

    // No hardware-correlated ALSA clock is provided at this stage. Report
    // unavailable instead of tricking the clock sync worker into false rebases.
    CHECK(output->position().source == AudioOutputPositionSource::Unavailable);
    output->reset();
    output->close();
    CHECK(output->open(plan.target_format) == AudioOutputOpenStatus::Ready);
    output->close();

    // Cancellation must never block behind the PCM device.
    stop.request_stop();
    CHECK(output->wait_until_writable(stop.get_token()) ==
        AudioOutputWaitStatus::Stopped);
    return 0;
}
