#include "audio/wasapi/wasapi_shared_audio_output_provider.hpp"

#if defined(_WIN32)

#include "audio/wasapi/wasapi_format.hpp"
#include "audio/wasapi/wasapi_shared_audio_output.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <utility>

namespace lapis::media::wasapi {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartmentLease final {
public:
    ComApartmentLease() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_initialization_ = SUCCEEDED(result_);
    }

    ~ComApartmentLease() {
        if (owns_initialization_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{E_FAIL};
    bool owns_initialization_{};
};

struct CoTaskMemDeleter final {
    template <typename T>
    void operator()(T* value) const noexcept {
        if (value != nullptr) {
            CoTaskMemFree(value);
        }
    }
};

template <typename T>
using CoTaskMemPtr = std::unique_ptr<T, CoTaskMemDeleter>;

bool device_unavailable(HRESULT result) noexcept {
    return result == AUDCLNT_E_DEVICE_INVALIDATED ||
        result == AUDCLNT_E_RESOURCES_INVALIDATED ||
        result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

AudioOutputPrepareStatus prepare_status_from_hresult(HRESULT result) noexcept {
    return device_unavailable(result)
        ? AudioOutputPrepareStatus::DeviceUnavailable
        : AudioOutputPrepareStatus::Faulted;
}

} // namespace

AudioOutputPlan WasapiSharedAudioOutputProvider::prepare_default_output() {
    ComApartmentLease apartment;
    if (!apartment.usable()) {
        return {AudioOutputPrepareStatus::Faulted};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    auto result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(result)) {
        return {AudioOutputPrepareStatus::Faulted};
    }

    ComPtr<IMMDevice> device;
    result = enumerator->GetDefaultAudioEndpoint(
        eRender,
        eMultimedia,
        device.GetAddressOf());
    if (FAILED(result)) {
        return {AudioOutputPrepareStatus::DeviceUnavailable};
    }

    LPWSTR endpoint_id_raw{};
    result = device->GetId(&endpoint_id_raw);
    CoTaskMemPtr<wchar_t> endpoint_id_owner(endpoint_id_raw);
    if (FAILED(result) || endpoint_id_owner == nullptr) {
        return {
            FAILED(result)
                ? prepare_status_from_hresult(result)
                : AudioOutputPrepareStatus::Faulted
        };
    }
    std::wstring endpoint_id(endpoint_id_owner.get());

    ComPtr<IAudioClient> audio_client;
    result = device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(audio_client.GetAddressOf()));
    if (FAILED(result)) {
        return {prepare_status_from_hresult(result)};
    }

    WAVEFORMATEX* mix_format_raw{};
    result = audio_client->GetMixFormat(&mix_format_raw);
    CoTaskMemPtr<WAVEFORMATEX> mix_format(mix_format_raw);
    if (FAILED(result) || mix_format == nullptr) {
        return {
            FAILED(result)
                ? prepare_status_from_hresult(result)
                : AudioOutputPrepareStatus::Faulted
        };
    }

    const auto target_format = parse_wave_format(*mix_format);
    if (!target_format.has_value()) {
        return {AudioOutputPrepareStatus::UnsupportedFormat};
    }

    const auto rebuilt = build_wave_format(*target_format);
    if (!rebuilt.has_value()) {
        return {AudioOutputPrepareStatus::UnsupportedFormat};
    }

    WAVEFORMATEX* closest{};
    result = audio_client->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED,
        &rebuilt->Format,
        &closest);
    if (closest != nullptr) {
        CoTaskMemFree(closest);
    }
    if (result != S_OK) {
        if (result == S_FALSE || result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            return {AudioOutputPrepareStatus::UnsupportedFormat};
        }
        return {prepare_status_from_hresult(result)};
    }

    AudioOutputPlan plan;
    plan.status = AudioOutputPrepareStatus::Ready;
    plan.target_format = *target_format;
    plan.output = std::make_unique<WasapiSharedAudioOutput>(
        std::move(endpoint_id));
    return plan;
}

} // namespace lapis::media::wasapi

#endif
