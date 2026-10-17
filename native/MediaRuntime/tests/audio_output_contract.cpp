#include "audio/audio_format_validation.hpp"
#include "audio/audio_output.hpp"
#include "audio/audio_output_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stop_token>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::cerr << "CHECK failed: " #x << " line " << __LINE__ << '\n'; return 1; } } while (false)

namespace {
class Buffer final : public lapis::media::IAudioBuffer {
public:
    lapis::media::AudioFormat spec{
        lapis::media::AudioSampleFormat::S16,
        lapis::media::AudioPacking::Interleaved,
        48000,
        {lapis::media::AudioChannel::FrontLeft, lapis::media::AudioChannel::FrontRight},
        false
    };
    std::uint32_t frames{4};
    std::uint32_t sample_bytes{2};
    std::vector<lapis::media::AudioPlaneView> views;
    [[nodiscard]] const lapis::media::AudioFormat& format() const noexcept override { return spec; }
    [[nodiscard]] std::uint32_t sample_frame_count() const noexcept override { return frames; }
    [[nodiscard]] std::uint32_t bytes_per_sample() const noexcept override { return sample_bytes; }
    [[nodiscard]] std::uint32_t plane_count() const noexcept override { return static_cast<std::uint32_t>(views.size()); }
    [[nodiscard]] lapis::media::AudioPlaneView plane(std::uint32_t i) const noexcept override {
        return i < views.size() ? views[i] : lapis::media::AudioPlaneView{};
    }
};

class NullAudioOutput final : public lapis::media::IAudioOutput {
public:
    [[nodiscard]] lapis::media::AudioOutputOpenStatus open(
        const lapis::media::AudioFormat&) override {
        return lapis::media::AudioOutputOpenStatus::Ready;
    }

    [[nodiscard]] lapis::media::AudioOutputSubmitResult try_submit(
        const lapis::media::AudioFrame&,
        std::uint32_t) override {
        return {lapis::media::AudioOutputStatus::Accepted, 1};
    }

    [[nodiscard]] lapis::media::AudioOutputWaitStatus wait_until_writable(
        std::stop_token) override {
        return lapis::media::AudioOutputWaitStatus::Ready;
    }

    [[nodiscard]] lapis::media::AudioOutputWaitStatus wait_until_drained(
        std::stop_token) override {
        return lapis::media::AudioOutputWaitStatus::Ready;
    }

    [[nodiscard]] lapis::media::AudioOutputControlStatus pause() override {
        return lapis::media::AudioOutputControlStatus::Ready;
    }

    [[nodiscard]] lapis::media::AudioOutputControlStatus resume() override {
        return lapis::media::AudioOutputControlStatus::Ready;
    }

    void reset() noexcept override {}

    [[nodiscard]] lapis::media::AudioOutputPosition position() const noexcept override {
        return {};
    }

    void close() noexcept override {}
};
}

int main() {
    using namespace lapis::media;
    std::array<std::byte, 32> storage{};
    Buffer buffer;
    buffer.views = {{storage.data(), 16}};
    CHECK(valid_audio_buffer(buffer));

    auto interleaved = audio_plane_slice(buffer, 0, 1, 2);
    CHECK(interleaved.has_value());
    CHECK(interleaved->data == storage.data() + 4);
    CHECK(interleaved->size_bytes == 8);
    CHECK(!audio_plane_slice(buffer, 1, 1, 2).has_value());
    CHECK(!audio_plane_slice(buffer, 0, 4, 1).has_value());
    CHECK(!audio_plane_slice(buffer, 0, 3, 2).has_value());
    CHECK(!audio_plane_slice(buffer, 0, 0, 0).has_value());

    buffer.views[0].size_bytes = 15;
    CHECK(!valid_audio_buffer(buffer));
    buffer.views[0].size_bytes = 16;
    buffer.sample_bytes = 4;
    CHECK(!valid_audio_buffer(buffer));
    buffer.sample_bytes = 2;
    buffer.spec.sample_rate = 0;
    CHECK(!valid_audio_buffer(buffer));
    buffer.spec.sample_rate = 48000;

    buffer.spec.packing = AudioPacking::Planar;
    buffer.views = {{storage.data(), 8}, {storage.data() + 8, 8}};
    CHECK(valid_audio_buffer(buffer));
    auto planar_left = audio_plane_slice(buffer, 0, 1, 2);
    auto planar_right = audio_plane_slice(buffer, 1, 1, 2);
    CHECK(planar_left.has_value());
    CHECK(planar_right.has_value());
    CHECK(planar_left->data == storage.data() + 2);
    CHECK(planar_left->size_bytes == 4);
    CHECK(planar_right->data == storage.data() + 10);
    CHECK(planar_right->size_bytes == 4);

    buffer.views.pop_back();
    CHECK(!valid_audio_buffer(buffer));
    buffer.views.push_back({nullptr, 8});
    CHECK(!valid_audio_buffer(buffer));
    buffer.views.back() = {storage.data() + 8, 8};
    buffer.spec.channels.clear();
    CHECK(!valid_audio_buffer(buffer));
    buffer.spec.channels = {AudioChannel::FrontLeft, AudioChannel::FrontRight};
    buffer.spec.sample_format = AudioSampleFormat::Unknown;
    CHECK(!valid_audio_buffer(buffer));

    const AudioFormat target{
        AudioSampleFormat::F32,
        AudioPacking::Interleaved,
        48'000,
        {AudioChannel::FrontLeft, AudioChannel::FrontRight},
        false
    };
    AudioOutputPlan ready_plan;
    ready_plan.status = AudioOutputPrepareStatus::Ready;
    ready_plan.target_format = target;
    ready_plan.output = std::make_unique<NullAudioOutput>();
    CHECK(ready_plan.ok());

    AudioOutputPlan missing_output;
    missing_output.status = AudioOutputPrepareStatus::Ready;
    missing_output.target_format = target;
    CHECK(!missing_output.ok());

    AudioOutputPlan unsupported;
    unsupported.status = AudioOutputPrepareStatus::UnsupportedFormat;
    unsupported.target_format = target;
    unsupported.output = std::make_unique<NullAudioOutput>();
    CHECK(!unsupported.ok());

    AudioOutputPlan missing_rate;
    missing_rate.status = AudioOutputPrepareStatus::Ready;
    missing_rate.target_format = target;
    missing_rate.target_format.sample_rate = 0;
    missing_rate.output = std::make_unique<NullAudioOutput>();
    CHECK(!missing_rate.ok());

    return 0;
}
