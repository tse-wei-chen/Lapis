#include "audio/alsa/alsa_pcm_audio_output.hpp"

#if !defined(__linux__)
#error ALSA output is a Linux-only platform adapter.
#endif

#include "audio/audio_format_validation.hpp"
#include "audio/alsa/alsa_playout_estimate.hpp"
#include "core/monotonic_time.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
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
        has_accepted_media_ = false;
        accepted_media_end_ = {};
        clear_position_cache(true);
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
        if (available < 0) {
            clear_position_cache(true);
            return {AudioOutputStatus::DeviceLost, 0};
        }
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
        if (written < 0) {
            clear_position_cache(true);
            return {AudioOutputStatus::DeviceLost, 0};
        }
        // ALSA has taken ownership of this exact prefix. Keep the canonical
        // media end rather than equating a device-sample counter with PTS.
        accepted_media_end_ = accepted_media_end(
            frame.timestamp,
            static_cast<std::uint64_t>(first) +
                static_cast<std::uint64_t>(written),
            frame.buffer->format().sample_rate);
        has_accepted_media_ = true;
        refresh_position_cache();
        return {AudioOutputStatus::Accepted,
            static_cast<std::uint32_t>(written)};
    }

    [[nodiscard]] AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (pcm_ == nullptr) return AudioOutputWaitStatus::DeviceLost;
            // Never block generation shutdown behind an ALSA device wait.
            const auto result = snd_pcm_wait(pcm_, 20);
            if (result < 0) {
                clear_position_cache(true);
                return AudioOutputWaitStatus::DeviceLost;
            }
            refresh_position_cache();
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
            if (result < 0) {
                clear_position_cache(true);
                return AudioOutputWaitStatus::DeviceLost;
            }
            refresh_position_cache_with_delay(pending);
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
        // No extrapolation is valid while the PCM device is paused.
        clear_position_cache(false);
        return AudioOutputControlStatus::Ready;
    }

    [[nodiscard]] AudioOutputControlStatus resume() {
        if (pcm_ == nullptr) return AudioOutputControlStatus::DeviceLost;
        if (!paused_) return AudioOutputControlStatus::Ready;
        if (snd_pcm_pause(pcm_, 0) < 0) return AudioOutputControlStatus::DeviceLost;
        paused_ = false;
        refresh_position_cache();
        return AudioOutputControlStatus::Ready;
    }

    void reset() noexcept {
        if (pcm_ == nullptr) return;
        (void)snd_pcm_drop(pcm_);
        (void)snd_pcm_prepare(pcm_);
        paused_ = false;
        has_accepted_media_ = false;
        accepted_media_end_ = {};
        clear_position_cache(true);
    }

    [[nodiscard]] AudioOutputPosition position() const noexcept {
        for (;;) {
            const auto first = position_sequence_.load(std::memory_order_acquire);
            if ((first & 1U) != 0) continue;
            const AudioOutputPosition result{
                MediaTime{presented_until_ns_.load(std::memory_order_relaxed)},
                MonotonicTime{sampled_at_ns_.load(std::memory_order_relaxed)},
                revision_.load(std::memory_order_relaxed),
                position_source_.load(std::memory_order_relaxed)
            };
            if (first == position_sequence_.load(std::memory_order_acquire))
                return result;
        }
    }

    void close() noexcept {
        if (pcm_ != nullptr) {
            (void)snd_pcm_drop(pcm_);
            (void)snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
        paused_ = false;
        has_accepted_media_ = false;
        accepted_media_end_ = {};
        clear_position_cache(true);
    }

private:
    // Only called on the audio render worker. The output clock reader runs
    // concurrently, but sees exclusively the atomic published snapshot.
    void refresh_position_cache() noexcept {
        if (pcm_ == nullptr || !has_accepted_media_ || paused_) return;
        snd_pcm_sframes_t pending{};
        if (snd_pcm_delay(pcm_, &pending) < 0) {
            clear_position_cache(false);
            return;
        }
        refresh_position_cache_with_delay(pending);
    }

    void refresh_position_cache_with_delay(snd_pcm_sframes_t pending) noexcept {
        if (!has_accepted_media_ || pending < 0 || paused_) {
            clear_position_cache(false);
            return;
        }
        // This is a QUEUE-DEPTH estimate at the time of the ALSA sample;
        // snd_pcm_delay is not a verified hardware timestamp. Never mark it
        // HardwareClock and thereby trigger false PlaybackClock rebases.
        const auto estimate = estimate_playout_position(
            accepted_media_end_, static_cast<std::uint64_t>(pending), 48'000);
        position_sequence_.fetch_add(1, std::memory_order_acq_rel);
        presented_until_ns_.store(estimate.nanoseconds, std::memory_order_relaxed);
        sampled_at_ns_.store(SteadyClockSource::now().nanoseconds,
            std::memory_order_relaxed);
        position_source_.store(AudioOutputPositionSource::Estimated,
            std::memory_order_relaxed);
        position_sequence_.fetch_add(1, std::memory_order_release);
    }

    void clear_position_cache(bool advance_revision) noexcept {
        position_sequence_.fetch_add(1, std::memory_order_acq_rel);
        presented_until_ns_.store(0, std::memory_order_relaxed);
        sampled_at_ns_.store(0, std::memory_order_relaxed);
        position_source_.store(AudioOutputPositionSource::Unavailable,
            std::memory_order_relaxed);
        if (advance_revision)
            revision_.fetch_add(1, std::memory_order_relaxed);
        position_sequence_.fetch_add(1, std::memory_order_release);
    }

    std::string device_;
    snd_pcm_t* pcm_{};
    bool paused_{};
    bool has_accepted_media_{};
    MediaTime accepted_media_end_{};
    mutable std::atomic<std::uint64_t> position_sequence_{};
    std::atomic<std::int64_t> presented_until_ns_{};
    std::atomic<std::uint64_t> sampled_at_ns_{};
    std::atomic<std::uint64_t> revision_{};
    std::atomic<AudioOutputPositionSource> position_source_{
        AudioOutputPositionSource::Unavailable};
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
        AudioOutputPlan unavailable;
        unavailable.status = AudioOutputPrepareStatus::DeviceUnavailable;
        return unavailable;
    }
    const auto negotiated = configure_stream(probe);
    snd_pcm_close(probe);
    if (negotiated < 0) {
        AudioOutputPlan rejected;
        rejected.status = negotiated == -EINVAL
            ? AudioOutputPrepareStatus::UnsupportedFormat
            : AudioOutputPrepareStatus::DeviceUnavailable;
        return rejected;
    }
    AudioOutputPlan plan;
    plan.status = AudioOutputPrepareStatus::Ready;
    plan.target_format = stereo_float_48k();
    plan.output = std::make_unique<AlsaPcmAudioOutput>(device);
    return plan;
}

} // namespace lapis::media::alsa
