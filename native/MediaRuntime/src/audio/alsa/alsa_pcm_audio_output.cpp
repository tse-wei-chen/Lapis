#include "audio/alsa/alsa_pcm_audio_output.hpp"

#if !defined(__linux__)
#error ALSA output is a Linux-only platform adapter.
#endif

#include "audio/audio_format_validation.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace lapis::media::alsa {
namespace {

// This exact target is negotiated in prepare_default_output() and rechecked
// at open(): no silent resampling/reordering in the device adapter.
AudioFormat stereo_float_48k() {
    return {AudioSampleFormat::F32, AudioPacking::Interleaved, 48'000,
        {AudioChannel::FrontLeft, AudioChannel::FrontRight}, false};
}

bool matches_target(const AudioFormat& format) noexcept {
    return format.sample_format == AudioSampleFormat::F32 &&
        format.packing == AudioPacking::Interleaved &&
        format.sample_rate == 48'000 &&
        format.channels.size() == 2 &&
        format.channels[0] == AudioChannel::FrontLeft &&
        format.channels[1] == AudioChannel::FrontRight;
}

int configure_stream(snd_pcm_t* pcm) noexcept {
    // ALSA nonblocking stream with a 100 ms bounded hardware/application
    // buffer. ALSA can be routed to PipeWire through the standard ALSA plugin.
    return snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48'000, 1, 100'000);
}

const char* selected_device() noexcept {
    const char* configured = std::getenv("LAPIS_ALSA_PCM");
    return configured != nullptr && *configured != '\0' ? configured : "default";
}

} // namespace

class AlsaPcmAudioOutput::Impl final {
public:
    explicit Impl(std::string name) : device_(std::move(name)) {}
    ~Impl() { close(); }

    [[nodiscard]] AudioOutputOpenStatus open(const AudioFormat& format) {
        close();
        if (!matches_target(format)) return AudioOutputOpenStatus::UnsupportedFormat;
        const auto open_result = snd_pcm_open(
            &pcm_, device_.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (open_result < 0 || pcm_ == nullptr) {
            close();
            return AudioOutputOpenStatus::DeviceLost;
        }
        const auto result = configure_stream(pcm_);
        if (result < 0) {
            close();
            return result == -EINVAL ? AudioOutputOpenStatus::UnsupportedFormat
                : AudioOutputOpenStatus::DeviceLost;
        }
        paused_ = false;
        return AudioOutputOpenStatus::Ready;
    }

    [[nodiscard]] AudioOutputSubmitResult try_submit(
        const AudioFrame& frame, std::uint32_t first) {
        if (pcm_ == nullptr || paused_ || frame.buffer == nullptr ||
            !valid_audio_buffer(*frame.buffer) ||
            !matches_target(frame.buffer->format()) ||
            first >= frame.buffer->sample_frame_count())
            return {AudioOutputStatus::Faulted, 0};

        const auto available = snd_pcm_avail_update(pcm_);
        if (available == -EAGAIN) return {AudioOutputStatus::Backpressure, 0};
        if (available < 0) return {AudioOutputStatus::DeviceLost, 0};
        if (available == 0) return {AudioOutputStatus::Backpressure, 0};

        const auto remaining = frame.buffer->sample_frame_count() - first;
        const auto count = static_cast<std::uint32_t>(
            std::min<snd_pcm_sframes_t>(available,
                std::min<std::uint32_t>(remaining, 2048)));
        if (count == 0) return {AudioOutputStatus::Backpressure, 0};
        const auto slice = audio_plane_slice(*frame.buffer, 0, first, count);
        if (!slice.has_value()) return {AudioOutputStatus::Faulted, 0};

        const auto written = snd_pcm_writei(pcm_, slice->data, count);
        if (written == -EAGAIN || written == 0)
            return {AudioOutputStatus::Backpressure, 0};
        if (written < 0)
            return {AudioOutputStatus::DeviceLost, 0};
        return {AudioOutputStatus::Accepted,
            static_cast<std::uint32_t>(written)};
    }

    [[nodiscard]] AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (pcm_ == nullptr) return AudioOutputWaitStatus::DeviceLost;
            // Never block generation shutdown behind an ALSA device wait.
            const auto result = snd_pcm_wait(pcm_, 20);
            if (result < 0) return AudioOutputWaitStatus::DeviceLost;
            if (result > 0) return AudioOutputWaitStatus::Ready;
        }
        return AudioOutputWaitStatus::Stopped;
    }

    [[nodiscard]] AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (pcm_ == nullptr) return AudioOutputWaitStatus::DeviceLost;
            snd_pcm_sframes_t pending{};
            const auto result = snd_pcm_delay(pcm_, &pending);
            if (result < 0) return AudioOutputWaitStatus::DeviceLost;
            if (pending <= 0) return AudioOutputWaitStatus::Ready;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return AudioOutputWaitStatus::Stopped;
    }

    [[nodiscard]] AudioOutputControlStatus pause() {
        if (pcm_ == nullptr) return AudioOutputControlStatus::DeviceLost;
        if (paused_) return AudioOutputControlStatus::Ready;
        // Never pretend to pause if a PCM plugin cannot preserve its buffer.
        if (snd_pcm_pause(pcm_, 1) < 0) return AudioOutputControlStatus::Faulted;
        paused_ = true;
        return AudioOutputControlStatus::Ready;
    }

    [[nodiscard]] AudioOutputControlStatus resume() {
        if (pcm_ == nullptr) return AudioOutputControlStatus::DeviceLost;
        if (!paused_) return AudioOutputControlStatus::Ready;
        if (snd_pcm_pause(pcm_, 0) < 0) return AudioOutputControlStatus::DeviceLost;
        paused_ = false;
        return AudioOutputControlStatus::Ready;
    }

    void reset() noexcept {
        if (pcm_ == nullptr) return;
        (void)snd_pcm_drop(pcm_);
        (void)snd_pcm_prepare(pcm_);
        paused_ = false;
    }

    [[nodiscard]] AudioOutputPosition position() const noexcept {
        // Do not pretend ALSA submission time is a hardware playout clock.
        // A future timestamp-correlated ALSA/PipeWire clock adapter will
        // explicitly publish HardwareClock when verified.
        return {};
    }

    void close() noexcept {
        if (pcm_ != nullptr) {
            (void)snd_pcm_drop(pcm_);
            (void)snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
        paused_ = false;
    }

private:
    std::string device_;
    snd_pcm_t* pcm_{};
    bool paused_{};
};

AlsaPcmAudioOutput::AlsaPcmAudioOutput(std::string device_name)
    : impl_(std::make_unique<Impl>(std::move(device_name))) {}
AlsaPcmAudioOutput::~AlsaPcmAudioOutput() = default;
AudioOutputOpenStatus AlsaPcmAudioOutput::open(const AudioFormat& format) {
    return impl_->open(format);
}
AudioOutputSubmitResult AlsaPcmAudioOutput::try_submit(
    const AudioFrame& frame, std::uint32_t first_sample_frame) {
    return impl_->try_submit(frame, first_sample_frame);
}
AudioOutputWaitStatus AlsaPcmAudioOutput::wait_until_writable(
    std::stop_token stop_token) {
    return impl_->wait_until_writable(stop_token);
}
AudioOutputWaitStatus AlsaPcmAudioOutput::wait_until_drained(
    std::stop_token stop_token) {
    return impl_->wait_until_drained(stop_token);
}
AudioOutputControlStatus AlsaPcmAudioOutput::pause() { return impl_->pause(); }
AudioOutputControlStatus AlsaPcmAudioOutput::resume() { return impl_->resume(); }
void AlsaPcmAudioOutput::reset() noexcept { impl_->reset(); }
AudioOutputPosition AlsaPcmAudioOutput::position() const noexcept {
    return impl_->position();
}
void AlsaPcmAudioOutput::close() noexcept { impl_->close(); }

AudioOutputPlan AlsaPcmAudioOutputProvider::prepare_default_output() {
    const std::string device(selected_device());
    snd_pcm_t* probe{};
    const auto opened = snd_pcm_open(
        &probe, device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (opened < 0 || probe == nullptr) {
        if (probe != nullptr) snd_pcm_close(probe);
        return {AudioOutputPrepareStatus::DeviceUnavailable};
    }
    const auto negotiated = configure_stream(probe);
    snd_pcm_close(probe);
    if (negotiated < 0) {
        return {negotiated == -EINVAL
            ? AudioOutputPrepareStatus::UnsupportedFormat
            : AudioOutputPrepareStatus::DeviceUnavailable};
    }
    AudioOutputPlan plan;
    plan.status = AudioOutputPrepareStatus::Ready;
    plan.target_format = stereo_float_48k();
    plan.output = std::make_unique<AlsaPcmAudioOutput>(device);
    return plan;
}

} // namespace lapis::media::alsa
