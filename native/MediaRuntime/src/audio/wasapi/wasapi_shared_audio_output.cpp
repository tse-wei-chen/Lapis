#include "audio/wasapi/wasapi_shared_audio_output.hpp"

#if defined(_WIN32)

#include "audio/audio_format_validation.hpp"
#include "audio/wasapi/wasapi_clock.hpp"
#include "audio/wasapi/wasapi_format.hpp"
#include "core/monotonic_time.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

// WASAPI/COM SDK headers must be initialized before the Win32 event-wait
// helper; including the helper earlier changes the Windows SDK GUID macros.
#include "audio/wasapi/wasapi_event_wait.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace lapis::media::wasapi {
namespace {

using Microsoft::WRL::ComPtr;

bool same_audio_format(const AudioFormat& left, const AudioFormat& right) {
    return left.sample_format == right.sample_format &&
        left.packing == right.packing &&
        left.sample_rate == right.sample_rate &&
        left.channels == right.channels;
}

bool device_unavailable(HRESULT result) noexcept {
    return result == AUDCLNT_E_DEVICE_INVALIDATED ||
        result == AUDCLNT_E_RESOURCES_INVALIDATED ||
        result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

AudioOutputOpenStatus open_status_from_hresult(HRESULT result) noexcept {
    if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        return AudioOutputOpenStatus::UnsupportedFormat;
    }
    return device_unavailable(result)
        ? AudioOutputOpenStatus::DeviceLost
        : AudioOutputOpenStatus::Faulted;
}

AudioOutputStatus submit_status_from_hresult(HRESULT result) noexcept {
    if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        return AudioOutputStatus::UnsupportedFormat;
    }
    return device_unavailable(result)
        ? AudioOutputStatus::DeviceLost
        : AudioOutputStatus::Faulted;
}

AudioOutputWaitStatus wait_status_from_hresult(HRESULT result) noexcept {
    return device_unavailable(result)
        ? AudioOutputWaitStatus::DeviceLost
        : AudioOutputWaitStatus::Faulted;
}

AudioOutputControlStatus control_status_from_hresult(HRESULT result) noexcept {
    return device_unavailable(result)
        ? AudioOutputControlStatus::DeviceLost
        : AudioOutputControlStatus::Faulted;
}

std::int64_t sample_frames_to_nanoseconds(
    std::uint64_t sample_frames,
    std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0) {
        return 0;
    }

    constexpr auto nanoseconds_per_second = std::uint64_t{1'000'000'000};
    if (sample_frames >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
            nanoseconds_per_second) {
        return std::numeric_limits<std::int64_t>::max();
    }

    return static_cast<std::int64_t>(
        (sample_frames * nanoseconds_per_second) / sample_rate);
}

std::int64_t saturating_add(std::int64_t left, std::int64_t right) noexcept {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

std::optional<std::uint64_t> current_qpc_100ns() noexcept {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (QueryPerformanceCounter(&counter) == FALSE ||
        QueryPerformanceFrequency(&frequency) == FALSE ||
        counter.QuadPart < 0 || frequency.QuadPart <= 0) {
        return std::nullopt;
    }

    return scale_clock_units(
        static_cast<std::uint64_t>(counter.QuadPart),
        static_cast<std::uint64_t>(frequency.QuadPart),
        10'000'000ULL);
}

} // namespace

class WasapiSharedAudioOutput::Impl final {
public:
    explicit Impl(std::wstring endpoint_id)
        : endpoint_id_(std::move(endpoint_id)) {}

    ~Impl() {
        close();
    }

    [[nodiscard]] AudioOutputOpenStatus open(const AudioFormat& format) {
        close();

        const auto wave = build_wave_format(format);
        if (!wave.has_value()) {
            return AudioOutputOpenStatus::UnsupportedFormat;
        }

        AudioFormat format_copy = format;

        const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
            return AudioOutputOpenStatus::Faulted;
        }
        com_initialized_ = SUCCEEDED(com_result);

        auto fail = [this](AudioOutputOpenStatus status) {
            close();
            return status;
        };

        auto result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator_.GetAddressOf()));
        if (FAILED(result)) {
            return fail(AudioOutputOpenStatus::Faulted);
        }

        if (endpoint_id_.empty()) {
            result = enumerator_->GetDefaultAudioEndpoint(
                eRender,
                eMultimedia,
                device_.GetAddressOf());
        } else {
            result = enumerator_->GetDevice(
                endpoint_id_.c_str(),
                device_.GetAddressOf());
        }
        if (FAILED(result)) {
            return fail(AudioOutputOpenStatus::DeviceLost);
        }

        result = device_->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(audio_client_.GetAddressOf()));
        if (FAILED(result)) {
            return fail(open_status_from_hresult(result));
        }

        WAVEFORMATEX* closest{};
        result = audio_client_->IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            &wave->Format,
            &closest);
        if (closest != nullptr) {
            CoTaskMemFree(closest);
        }
        if (result != S_OK) {
            if (result == S_FALSE || result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
                return fail(AudioOutputOpenStatus::UnsupportedFormat);
            }
            return fail(open_status_from_hresult(result));
        }

        constexpr DWORD stream_flags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_NOPERSIST;
        result = audio_client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            stream_flags,
            0,
            0,
            &wave->Format,
            nullptr);
        if (FAILED(result)) {
            return fail(open_status_from_hresult(result));
        }

        audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (audio_event_ == nullptr || stop_event_ == nullptr) {
            return fail(AudioOutputOpenStatus::Faulted);
        }

        result = audio_client_->SetEventHandle(audio_event_);
        if (FAILED(result)) {
            return fail(open_status_from_hresult(result));
        }

        result = audio_client_->GetBufferSize(&buffer_frame_count_);
        if (FAILED(result) || buffer_frame_count_ == 0) {
            return fail(
                FAILED(result)
                    ? open_status_from_hresult(result)
                    : AudioOutputOpenStatus::Faulted);
        }

        result = audio_client_->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_client_.GetAddressOf()));
        if (FAILED(result)) {
            return fail(open_status_from_hresult(result));
        }

        result = audio_client_->GetService(
            __uuidof(IAudioClock),
            reinterpret_cast<void**>(audio_clock_.GetAddressOf()));
        if (FAILED(result)) {
            return fail(open_status_from_hresult(result));
        }

        UINT64 clock_frequency{};
        result = audio_clock_->GetFrequency(&clock_frequency);
        if (FAILED(result) || clock_frequency == 0) {
            return fail(
                FAILED(result)
                    ? open_status_from_hresult(result)
                    : AudioOutputOpenStatus::Faulted);
        }

        active_format_ = std::move(format_copy);
        sample_rate_ = active_format_.sample_rate;
        audio_clock_frequency_ = clock_frequency;
        stream_origin_media_ns_ = 0;
        has_stream_origin_ = false;
        has_started_ = false;
        running_ = false;
        paused_ = false;
        opened_ = true;
        clear_position_cache(true);
        return AudioOutputOpenStatus::Ready;
    }

    [[nodiscard]] AudioOutputSubmitResult try_submit(
        const AudioFrame& frame,
        std::uint32_t first_sample_frame) {
        if (!opened_ || paused_ ||
            audio_client_.Get() == nullptr ||
            render_client_.Get() == nullptr ||
            audio_clock_.Get() == nullptr ||
            frame.buffer == nullptr ||
            !valid_audio_buffer(*frame.buffer) ||
            !same_audio_format(active_format_, frame.buffer->format())) {
            return {AudioOutputStatus::Faulted, 0};
        }

        const auto total_sample_frames = frame.buffer->sample_frame_count();
        if (first_sample_frame >= total_sample_frames) {
            return {AudioOutputStatus::Faulted, 0};
        }

        UINT32 padding{};
        auto result = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(result)) {
            return {submit_status_from_hresult(result), 0};
        }
        if (padding > buffer_frame_count_) {
            return {AudioOutputStatus::Faulted, 0};
        }

        const auto writable = buffer_frame_count_ - padding;
        if (writable == 0) {
            refresh_position_cache();
            return {AudioOutputStatus::Backpressure, 0};
        }

        const auto remaining = total_sample_frames - first_sample_frame;
        const auto accepted = std::min(remaining, writable);
        const auto slice = audio_plane_slice(
            *frame.buffer,
            0,
            first_sample_frame,
            accepted);
        if (!slice.has_value()) {
            return {AudioOutputStatus::Faulted, 0};
        }

        BYTE* destination{};
        result = render_client_->GetBuffer(accepted, &destination);
        if (FAILED(result) || destination == nullptr) {
            return {
                FAILED(result)
                    ? submit_status_from_hresult(result)
                    : AudioOutputStatus::Faulted,
                0
            };
        }

        std::memcpy(destination, slice->data, slice->size_bytes);
        result = render_client_->ReleaseBuffer(accepted, 0);
        if (FAILED(result)) {
            return {submit_status_from_hresult(result), 0};
        }

        if (!has_stream_origin_) {
            stream_origin_media_ns_ = saturating_add(
                frame.timestamp.nanoseconds,
                sample_frames_to_nanoseconds(first_sample_frame, sample_rate_));
            has_stream_origin_ = true;
        }

        if (!has_started_) {
            result = audio_client_->Start();
            if (FAILED(result)) {
                // ReleaseBuffer already transferred the prefix to the endpoint.
                // Reset it before reporting a zero-consumption error so the
                // IAudioOutput partial-submission contract remains truthful.
                (void)audio_client_->Stop();
                (void)audio_client_->Reset();
                has_stream_origin_ = false;
                stream_origin_media_ns_ = 0;
                clear_position_cache(true);
                return {submit_status_from_hresult(result), 0};
            }
            has_started_ = true;
            running_ = true;
        }

        refresh_position_cache();
        return {AudioOutputStatus::Accepted, accepted};
    }

    [[nodiscard]] AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) {
        if (!opened_ || audio_event_ == nullptr || stop_event_ == nullptr) {
            return AudioOutputWaitStatus::Faulted;
        }
        if (stop_token.stop_requested() || paused_) {
            return AudioOutputWaitStatus::Stopped;
        }
        if (!running_) {
            return AudioOutputWaitStatus::Ready;
        }
        return detail::wait_for_writable_or_device_loss(
            audio_event_, stop_event_, stop_token, [this] {
                // Endpoint unplug/default-device transitions may silence the
                // event indefinitely. Probe GetCurrentPadding at most once
                // every 100 ms while the event isn't firing.
                UINT32 padding{};
                const auto result = audio_client_->GetCurrentPadding(&padding);
                if (FAILED(result)) {
                    return device_unavailable(result)
                        ? detail::WritableProbeResult::DeviceLost
                        : detail::WritableProbeResult::Faulted;
                }
                if (padding >= buffer_frame_count_) {
                    return detail::WritableProbeResult::Pending;
                }
                refresh_position_cache();
                return detail::WritableProbeResult::Writable;
            });
    }

    [[nodiscard]] AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) {
        if (!opened_ || audio_client_.Get() == nullptr) {
            return AudioOutputWaitStatus::Faulted;
        }

        for (;;) {
            if (stop_token.stop_requested() || paused_) {
                return AudioOutputWaitStatus::Stopped;
            }

            UINT32 padding{};
            const auto result = audio_client_->GetCurrentPadding(&padding);
            if (FAILED(result)) {
                return wait_status_from_hresult(result);
            }
            refresh_position_cache();
            if (padding == 0) {
                return AudioOutputWaitStatus::Ready;
            }

            const auto wait_status = wait_until_writable(stop_token);
            if (wait_status != AudioOutputWaitStatus::Ready) {
                return wait_status;
            }
        }
    }

    [[nodiscard]] AudioOutputControlStatus pause() {
        if (!opened_ || audio_client_.Get() == nullptr) {
            return AudioOutputControlStatus::Faulted;
        }
        if (paused_) {
            return AudioOutputControlStatus::Ready;
        }

        if (running_) {
            const auto result = audio_client_->Stop();
            if (FAILED(result)) {
                return control_status_from_hresult(result);
            }
            running_ = false;
            refresh_position_cache();
        }

        paused_ = true;
        return AudioOutputControlStatus::Ready;
    }

    [[nodiscard]] AudioOutputControlStatus resume() {
        if (!opened_ || audio_client_.Get() == nullptr) {
            return AudioOutputControlStatus::Faulted;
        }
        if (!paused_) {
            return AudioOutputControlStatus::Ready;
        }

        if (has_started_ && !running_) {
            const auto result = audio_client_->Start();
            if (FAILED(result)) {
                return control_status_from_hresult(result);
            }
            running_ = true;
            refresh_position_cache();
        }

        paused_ = false;
        return AudioOutputControlStatus::Ready;
    }

    void reset() noexcept {
        if (audio_client_.Get() != nullptr) {
            if (running_) {
                (void)audio_client_->Stop();
            }
            (void)audio_client_->Reset();
        }

        has_stream_origin_ = false;
        stream_origin_media_ns_ = 0;
        has_started_ = false;
        running_ = false;
        paused_ = false;
        clear_position_cache(true);
    }

    [[nodiscard]] AudioOutputPosition position() const noexcept {
        for (;;) {
            const auto first_sequence =
                position_sequence_.load(std::memory_order_acquire);
            if ((first_sequence & 1U) != 0U) {
                continue;
            }

            const AudioOutputPosition snapshot{
                MediaTime{presented_until_ns_.load(std::memory_order_relaxed)},
                MonotonicTime{sampled_at_ns_.load(std::memory_order_relaxed)},
                revision_.load(std::memory_order_relaxed),
                position_source_.load(std::memory_order_relaxed)
            };

            const auto second_sequence =
                position_sequence_.load(std::memory_order_acquire);
            if (first_sequence == second_sequence) {
                return snapshot;
            }
        }
    }

    void close() noexcept {
        if (audio_client_.Get() != nullptr && running_) {
            (void)audio_client_->Stop();
        }

        audio_clock_.Reset();
        render_client_.Reset();
        audio_client_.Reset();
        device_.Reset();
        enumerator_.Reset();

        if (audio_event_ != nullptr) {
            CloseHandle(audio_event_);
            audio_event_ = nullptr;
        }
        if (stop_event_ != nullptr) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }

        opened_ = false;
        has_stream_origin_ = false;
        has_started_ = false;
        running_ = false;
        paused_ = false;
        buffer_frame_count_ = 0;
        sample_rate_ = 0;
        audio_clock_frequency_ = 0;
        stream_origin_media_ns_ = 0;
        clear_position_cache(false);

        if (com_initialized_) {
            CoUninitialize();
            com_initialized_ = false;
        }
    }

private:
    void refresh_position_cache() noexcept {
        if (audio_clock_.Get() == nullptr ||
            !has_stream_origin_ ||
            audio_clock_frequency_ == 0) {
            return;
        }

        UINT64 device_position{};
        UINT64 sampled_qpc_100ns{};
        const auto result = audio_clock_->GetPosition(
            &device_position,
            &sampled_qpc_100ns);
        if (FAILED(result)) {
            return;
        }

        const auto current_qpc = current_qpc_100ns();
        if (!current_qpc.has_value()) {
            return;
        }
        const auto current_monotonic = SteadyClockSource::now();
        const auto sampled_at = correlate_qpc_100ns_to_monotonic(
            sampled_qpc_100ns,
            *current_qpc,
            current_monotonic);
        const auto media_position = media_time_from_audio_clock(
            MediaTime{stream_origin_media_ns_},
            device_position,
            audio_clock_frequency_);
        publish_position(
            media_position,
            sampled_at,
            AudioOutputPositionSource::HardwareClock);
    }

    void publish_position(
        MediaTime media_position,
        MonotonicTime sampled_at,
        AudioOutputPositionSource source) noexcept {
        position_sequence_.fetch_add(1, std::memory_order_acq_rel);
        presented_until_ns_.store(
            media_position.nanoseconds,
            std::memory_order_relaxed);
        sampled_at_ns_.store(
            sampled_at.nanoseconds,
            std::memory_order_relaxed);
        position_source_.store(source, std::memory_order_relaxed);
        position_sequence_.fetch_add(1, std::memory_order_release);
    }

    void clear_position_cache(bool advance_revision) noexcept {
        position_sequence_.fetch_add(1, std::memory_order_acq_rel);
        presented_until_ns_.store(0, std::memory_order_relaxed);
        sampled_at_ns_.store(0, std::memory_order_relaxed);
        position_source_.store(
            AudioOutputPositionSource::Unavailable,
            std::memory_order_relaxed);
        if (advance_revision) {
            revision_.fetch_add(1, std::memory_order_relaxed);
        }
        position_sequence_.fetch_add(1, std::memory_order_release);
    }

    std::wstring endpoint_id_;
    bool com_initialized_{};
    bool opened_{};
    bool has_stream_origin_{};
    bool has_started_{};
    bool running_{};
    bool paused_{};
    UINT32 buffer_frame_count_{};
    std::uint32_t sample_rate_{};
    std::uint64_t audio_clock_frequency_{};
    std::int64_t stream_origin_media_ns_{};
    AudioFormat active_format_{};

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioRenderClient> render_client_;
    ComPtr<IAudioClock> audio_clock_;
    HANDLE audio_event_{};
    HANDLE stop_event_{};

    mutable std::atomic<std::uint64_t> position_sequence_{};
    std::atomic<std::int64_t> presented_until_ns_{};
    std::atomic<std::uint64_t> sampled_at_ns_{};
    std::atomic<std::uint64_t> revision_{};
    std::atomic<AudioOutputPositionSource> position_source_{
        AudioOutputPositionSource::Unavailable
    };
};

WasapiSharedAudioOutput::WasapiSharedAudioOutput()
    : impl_(std::make_unique<Impl>(std::wstring{})) {}

WasapiSharedAudioOutput::WasapiSharedAudioOutput(std::wstring endpoint_id)
    : impl_(std::make_unique<Impl>(std::move(endpoint_id))) {}

WasapiSharedAudioOutput::~WasapiSharedAudioOutput() = default;

AudioOutputOpenStatus WasapiSharedAudioOutput::open(
    const AudioFormat& format) {
    return impl_->open(format);
}

AudioOutputSubmitResult WasapiSharedAudioOutput::try_submit(
    const AudioFrame& frame,
    std::uint32_t first_sample_frame) {
    return impl_->try_submit(frame, first_sample_frame);
}

AudioOutputWaitStatus WasapiSharedAudioOutput::wait_until_writable(
    std::stop_token stop_token) {
    return impl_->wait_until_writable(stop_token);
}

AudioOutputWaitStatus WasapiSharedAudioOutput::wait_until_drained(
    std::stop_token stop_token) {
    return impl_->wait_until_drained(stop_token);
}

AudioOutputControlStatus WasapiSharedAudioOutput::pause() {
    return impl_->pause();
}

AudioOutputControlStatus WasapiSharedAudioOutput::resume() {
    return impl_->resume();
}

void WasapiSharedAudioOutput::reset() noexcept {
    impl_->reset();
}

AudioOutputPosition WasapiSharedAudioOutput::position() const noexcept {
    return impl_->position();
}

void WasapiSharedAudioOutput::close() noexcept {
    impl_->close();
}

} // namespace lapis::media::wasapi

#endif
