#include "audio/wasapi/wasapi_shared_audio_output.hpp"

#if defined(_WIN32)

#include "audio/audio_format_validation.hpp"
#include "audio/wasapi/wasapi_format.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
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

        active_format_ = std::move(format_copy);
        sample_rate_ = active_format_.sample_rate;
        submitted_until_ns_ = 0;
        started_ = false;
        opened_ = true;
        presented_until_ns_.store(0, std::memory_order_release);
        revision_.fetch_add(1, std::memory_order_acq_rel);
        return AudioOutputOpenStatus::Ready;
    }

    [[nodiscard]] AudioOutputSubmitResult try_submit(
        const AudioFrame& frame,
        std::uint32_t first_sample_frame) {
        if (!opened_ ||
            audio_client_.Get() == nullptr ||
            render_client_.Get() == nullptr ||
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
            update_position_from_padding(padding);
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

        const auto submitted_frame_index =
            static_cast<std::uint64_t>(first_sample_frame) + accepted;
        submitted_until_ns_ = saturating_add(
            frame.timestamp.nanoseconds,
            sample_frames_to_nanoseconds(
                submitted_frame_index,
                sample_rate_));

        if (!started_) {
            result = audio_client_->Start();
            if (FAILED(result)) {
                return {submit_status_from_hresult(result), 0};
            }
            started_ = true;
        }

        refresh_position_cache();
        return {AudioOutputStatus::Accepted, accepted};
    }

    [[nodiscard]] AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) {
        if (!opened_ || audio_event_ == nullptr || stop_event_ == nullptr) {
            return AudioOutputWaitStatus::Faulted;
        }
        if (stop_token.stop_requested()) {
            return AudioOutputWaitStatus::Stopped;
        }
        if (!started_) {
            return AudioOutputWaitStatus::Ready;
        }
        if (ResetEvent(stop_event_) == FALSE) {
            return AudioOutputWaitStatus::Faulted;
        }

        std::stop_callback stop_callback(
            stop_token,
            [event = stop_event_]() noexcept {
                (void)SetEvent(event);
            });

        const HANDLE handles[] = {audio_event_, stop_event_};
        const auto wait_result = WaitForMultipleObjects(
            2,
            handles,
            FALSE,
            INFINITE);

        if (stop_token.stop_requested() ||
            wait_result == WAIT_OBJECT_0 + 1) {
            return AudioOutputWaitStatus::Stopped;
        }
        if (wait_result != WAIT_OBJECT_0) {
            return AudioOutputWaitStatus::Faulted;
        }

        UINT32 padding{};
        const auto result = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(result)) {
            return wait_status_from_hresult(result);
        }
        update_position_from_padding(padding);
        return AudioOutputWaitStatus::Ready;
    }

    [[nodiscard]] AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) {
        if (!opened_ || audio_client_.Get() == nullptr) {
            return AudioOutputWaitStatus::Faulted;
        }

        for (;;) {
            if (stop_token.stop_requested()) {
                return AudioOutputWaitStatus::Stopped;
            }

            UINT32 padding{};
            const auto result = audio_client_->GetCurrentPadding(&padding);
            if (FAILED(result)) {
                return wait_status_from_hresult(result);
            }
            update_position_from_padding(padding);
            if (padding == 0) {
                return AudioOutputWaitStatus::Ready;
            }

            const auto wait_status = wait_until_writable(stop_token);
            if (wait_status != AudioOutputWaitStatus::Ready) {
                return wait_status;
            }
        }
    }

    void reset() noexcept {
        if (audio_client_.Get() != nullptr) {
            if (started_) {
                (void)audio_client_->Stop();
            }
            (void)audio_client_->Reset();
        }

        started_ = false;
        submitted_until_ns_ = 0;
        presented_until_ns_.store(0, std::memory_order_release);
        revision_.fetch_add(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] AudioOutputPosition position() const noexcept {
        return {
            MediaTime{
                presented_until_ns_.load(std::memory_order_acquire)
            },
            revision_.load(std::memory_order_acquire)
        };
    }

    void close() noexcept {
        if (audio_client_.Get() != nullptr && started_) {
            (void)audio_client_->Stop();
        }

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
        started_ = false;
        buffer_frame_count_ = 0;
        sample_rate_ = 0;
        submitted_until_ns_ = 0;

        if (com_initialized_) {
            CoUninitialize();
            com_initialized_ = false;
        }
    }

private:
    void refresh_position_cache() noexcept {
        if (audio_client_.Get() == nullptr) {
            return;
        }

        UINT32 padding{};
        if (SUCCEEDED(audio_client_->GetCurrentPadding(&padding))) {
            update_position_from_padding(padding);
        }
    }

    void update_position_from_padding(UINT32 padding) noexcept {
        if (sample_rate_ == 0) {
            return;
        }

        const auto queued_duration = sample_frames_to_nanoseconds(
            padding,
            sample_rate_);
        const auto presented =
            submitted_until_ns_ >= queued_duration
            ? submitted_until_ns_ - queued_duration
            : 0;
        presented_until_ns_.store(presented, std::memory_order_release);
    }

    std::wstring endpoint_id_;
    bool com_initialized_{};
    bool opened_{};
    bool started_{};
    UINT32 buffer_frame_count_{};
    std::uint32_t sample_rate_{};
    AudioFormat active_format_{};
    std::int64_t submitted_until_ns_{};

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioRenderClient> render_client_;
    HANDLE audio_event_{};
    HANDLE stop_event_{};

    std::atomic<std::int64_t> presented_until_ns_{};
    std::atomic<std::uint64_t> revision_{};
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
