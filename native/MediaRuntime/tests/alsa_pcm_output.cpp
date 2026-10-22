#include "audio/alsa/alsa_pcm_audio_output.hpp"
#include "audio/alsa/alsa_playout_estimate.hpp"

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

    // Checked arithmetic, no dependence on any device timing semantics.
    CHECK(accepted_media_end(MediaTime{1'000'000'000}, 480, 48'000)
        == MediaTime{1'010'000'000});
    CHECK(estimate_playout_position(MediaTime{2'000'000'000}, 240, 48'000)
        == MediaTime{1'995'000'000});
    CHECK(estimate_playout_position(MediaTime{2'000'000}, 480, 48'000)
        == MediaTime{0});
    CHECK(accepted_media_end(MediaTime{0}, UINT64_MAX, 48'000)
        == MediaTime{INT64_MAX});
    CHECK(estimate_playout_position(MediaTime{0}, 0, 0) == MediaTime{0});

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
    const auto initial_revision = output->position().revision;
    CHECK(output->position().source == AudioOutputPositionSource::Unavailable);

    auto unsupported = plan.target_format;
    unsupported.sample_rate = 44'100;
    CHECK(output->open(unsupported) == AudioOutputOpenStatus::UnsupportedFormat);
    CHECK(output->open(plan.target_format) == AudioOutputOpenStatus::Ready);
    CHECK(output->position().revision > initial_revision);

    AudioFrame frame;
    frame.buffer = std::make_shared<TestBuffer>();
    frame.timestamp = MediaTime{5'000'000'000};
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
    const auto estimated = output->position();
    CHECK(estimated.source == AudioOutputPositionSource::Estimated);
    CHECK(estimated.sampled_at.nanoseconds != 0);
    CHECK(estimated.presented_until.nanoseconds >= 5'000'000'000);
    CHECK(estimated.presented_until.nanoseconds <= 5'010'000'000);
    CHECK(output->wait_until_drained(stop.get_token()) ==
        AudioOutputWaitStatus::Ready);

    // Queue-depth correlation is intentionally *Estimated*, never a
    // HardwareClock input to the native AV clock discipline.
    CHECK(output->position().source == AudioOutputPositionSource::Estimated);
    const auto before_reset = output->position().revision;
    output->reset();
    const auto after_reset = output->position();
    CHECK(after_reset.source == AudioOutputPositionSource::Unavailable);
    CHECK(after_reset.revision > before_reset);
    output->close();
    CHECK(output->position().source == AudioOutputPositionSource::Unavailable);
    CHECK(output->open(plan.target_format) == AudioOutputOpenStatus::Ready);
    CHECK(output->position().source == AudioOutputPositionSource::Unavailable);
    output->close();

    // Cancellation must never block behind the PCM device.
    stop.request_stop();
    CHECK(output->wait_until_writable(stop.get_token()) ==
        AudioOutputWaitStatus::Stopped);
    return 0;
}
