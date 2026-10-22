#include "video/d3d11/d3d11_video_presenter.hpp"

#if !defined(_WIN32)
#error D3D11VideoPresenter must only be built on Windows.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>

namespace lapis::media::d3d11 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr char vertex_shader_source[] = R"(
struct Input {
    float2 position : POSITION;
};
struct Output {
    float4 position : SV_POSITION;
};
Output main(Input input) {
    Output o;
    o.position = float4(input.position, 0.0f, 1.0f);
    return o;
}
)";

constexpr char pixel_shader_source[] = R"(
Texture2D video_frame : register(t0);
SamplerState linear_sampler : register(s0);
cbuffer ViewportConstants : register(b0) {
    float4 video_viewport;
};
float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = (position.xy - video_viewport.xy) / video_viewport.zw;
    return video_frame.Sample(linear_sampler, uv);
}
)";

struct VideoVertex final {
    float position[2];
};

constexpr std::array<VideoVertex, 6> video_vertices{{
    {{-1.0f, 1.0f}},
    {{1.0f, 1.0f}},
    {{-1.0f, -1.0f}},
    {{-1.0f, -1.0f}},
    {{1.0f, 1.0f}},
    {{1.0f, -1.0f}}
}};

[[nodiscard]] bool device_removed(HRESULT hr) noexcept {
    return hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_HUNG;
}

[[nodiscard]] VideoPresentStatus failed_present(HRESULT hr) noexcept {
    return device_removed(hr)
        ? VideoPresentStatus::DeviceLost
        : VideoPresentStatus::Failed;
}

[[nodiscard]] const char* failure_stage_name(
    VideoPresenterFailureStage stage) noexcept {
    switch (stage) {
        case VideoPresenterFailureStage::DeviceCreation: return "device_create";
        case VideoPresenterFailureStage::AdapterDiscovery: return "adapter_lookup";
        case VideoPresenterFailureStage::WindowQuery: return "window_query";
        case VideoPresenterFailureStage::SwapChainCreation: return "swapchain_create";
        case VideoPresenterFailureStage::RenderTargetCreation: return "render_target";
        case VideoPresenterFailureStage::ShaderCreation: return "shader_create";
        case VideoPresenterFailureStage::ResizeBuffers: return "swapchain_resize";
        case VideoPresenterFailureStage::TextureUpload: return "texture_upload";
        case VideoPresenterFailureStage::TextureMap: return "texture_map";
        case VideoPresenterFailureStage::Present: return "present";
        case VideoPresenterFailureStage::None: return "none";
    }
    return "unknown";
}

} // namespace

class D3D11VideoPresenter::Impl final {
public:
    ~Impl() { release(); }

    void set_journal(std::shared_ptr<DiagnosticEventJournal> journal) noexcept {
        journal_ = std::move(journal);
    }

    void set_epoch(std::uint64_t epoch) noexcept {
        current_epoch_.store(epoch, std::memory_order_relaxed);
    }

    [[nodiscard]] VideoPresenterDiagnostics diagnostics() const noexcept {
        return {
            device_creations_.load(std::memory_order_relaxed),
            swap_chain_creations_.load(std::memory_order_relaxed),
            swap_chain_resizes_.load(std::memory_order_relaxed),
            device_loss_events_.load(std::memory_order_relaxed),
            last_hresult_.load(std::memory_order_relaxed),
            last_removed_reason_.load(std::memory_order_relaxed),
            last_stage_.load(std::memory_order_acquire)
        };
    }

    [[nodiscard]] VideoOutputSurfaceApplyStatus bind(
        VideoOutputSurfaceDescriptor surface) {
        if (!valid_video_output_surface(surface)) {
            return VideoOutputSurfaceApplyStatus::Failed;
        }

        if (surface.kind == VideoOutputSurfaceKind::None) {
            release();
            return VideoOutputSurfaceApplyStatus::Ready;
        }
        if (surface.kind != VideoOutputSurfaceKind::Win32Window) {
            return VideoOutputSurfaceApplyStatus::Unsupported;
        }

        const auto next_hwnd = reinterpret_cast<HWND>(surface.native_handle);
        if (next_hwnd == hwnd_ && swap_chain_.Get() != nullptr) {
            return VideoOutputSurfaceApplyStatus::Ready;
        }

        // Release all resources owned by the old HWND before acknowledgement.
        // This is called only on the worker's rendering thread, never the UI.
        release();
        if (!IsWindow(next_hwnd)) {
            record_failure(
                VideoPresenterFailureStage::WindowQuery,
                HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }

        const auto result = initialize(next_hwnd);
        if (result != VideoOutputSurfaceApplyStatus::Ready) {
            release();
        }
        return result;
    }

    void release() noexcept {
        if (context_.Get() != nullptr) {
            context_->ClearState();
            context_->Flush();
        }
        upload_view_.Reset();
        upload_texture_.Reset();
        render_target_.Reset();
        swap_chain_.Reset();
        sampler_.Reset();
        vertex_buffer_.Reset();
        viewport_buffer_.Reset();
        input_layout_.Reset();
        pixel_shader_.Reset();
        vertex_shader_.Reset();
        context_.Reset();
        device_.Reset();
        hwnd_ = nullptr;
        back_width_ = 0;
        back_height_ = 0;
        upload_width_ = 0;
        upload_height_ = 0;
    }

    [[nodiscard]] VideoPresentStatus draw(const VideoFrame& frame) {
        // Retain headless playback for unbound sessions (including audio-only).
        if (hwnd_ == nullptr) {
            return VideoPresentStatus::Presented;
        }
        if (!IsWindow(hwnd_)) {
            record_failure(
                VideoPresenterFailureStage::WindowQuery,
                HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
            return VideoPresentStatus::DeviceLost;
        }
        if (frame.surface == nullptr ||
            frame.surface->kind() != VideoSurfaceKind::SystemMemory ||
            frame.surface->pixel_format() != VideoPixelFormat::Bgra8) {
            return VideoPresentStatus::UnsupportedSurface;
        }
        const auto* source = dynamic_cast<const ISystemMemoryVideoSurface*>(
            frame.surface.get());
        if (source == nullptr || source->plane_count() != 1 ||
            source->width() == 0 || source->height() == 0 ||
            source->width() > std::numeric_limits<std::uint32_t>::max() / 4) {
            return VideoPresentStatus::UnsupportedSurface;
        }

        const auto plane = source->plane(0);
        const auto row_bytes = static_cast<std::size_t>(source->width()) * 4;
        if (plane.data == nullptr || plane.stride <= 0 ||
            static_cast<std::uint64_t>(plane.stride) < row_bytes) {
            return VideoPresentStatus::UnsupportedSurface;
        }

        RECT client{};
        if (!GetClientRect(hwnd_, &client)) {
            record_failure(
                VideoPresenterFailureStage::WindowQuery,
                HRESULT_FROM_WIN32(GetLastError()));
            return VideoPresentStatus::DeviceLost;
        }
        if (client.right <= client.left || client.bottom <= client.top) {
            // A minimized/zero-size window is not an error or a device loss.
            return VideoPresentStatus::Presented;
        }

        const auto window_width = static_cast<UINT>(client.right - client.left);
        const auto window_height = static_cast<UINT>(client.bottom - client.top);
        if (window_width != back_width_ || window_height != back_height_) {
            const auto resize_hr = resize(window_width, window_height);
            if (FAILED(resize_hr)) {
                return failed_present(resize_hr);
            }
        }

        const auto texture_hr = ensure_upload(source->width(), source->height());
        if (FAILED(texture_hr)) {
            record_failure(VideoPresenterFailureStage::TextureUpload, texture_hr);
            return failed_present(texture_hr);
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto map_hr = context_->Map(
            upload_texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(map_hr)) {
            record_failure(VideoPresenterFailureStage::TextureMap, map_hr);
            return failed_present(map_hr);
        }

        if (mapped.RowPitch < row_bytes) {
            context_->Unmap(upload_texture_.Get(), 0);
            return VideoPresentStatus::Failed;
        }
        auto* destination = static_cast<std::byte*>(mapped.pData);
        for (std::uint32_t row = 0; row < source->height(); ++row) {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                plane.data + static_cast<std::ptrdiff_t>(row) * plane.stride,
                row_bytes);
        }
        context_->Unmap(upload_texture_.Get(), 0);

        ID3D11RenderTargetView* target = render_target_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        constexpr float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(target, black);

        // Preserve source aspect ratio instead of stretching anamorphically.
        const auto scale = std::min(
            static_cast<float>(window_width) / static_cast<float>(source->width()),
            static_cast<float>(window_height) / static_cast<float>(source->height()));
        const auto view_width = scale * static_cast<float>(source->width());
        const auto view_height = scale * static_cast<float>(source->height());
        const D3D11_VIEWPORT viewport{
            (static_cast<float>(window_width) - view_width) * 0.5f,
            (static_cast<float>(window_height) - view_height) * 0.5f,
            view_width,
            view_height,
            0.0f,
            1.0f
        };
        D3D11_MAPPED_SUBRESOURCE mapped_viewport{};
        const auto viewport_map_hr = context_->Map(
            viewport_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
            &mapped_viewport);
        if (FAILED(viewport_map_hr)) {
            record_failure(
                VideoPresenterFailureStage::TextureMap, viewport_map_hr);
            return failed_present(viewport_map_hr);
        }
        const std::array<float, 4> viewport_values{
            viewport.TopLeftX, viewport.TopLeftY,
            viewport.Width, viewport.Height
        };
        std::memcpy(
            mapped_viewport.pData, viewport_values.data(),
            sizeof(viewport_values));
        context_->Unmap(viewport_buffer_.Get(), 0);

        context_->RSSetViewports(1, &viewport);
        constexpr UINT vertex_stride = sizeof(VideoVertex);
        constexpr UINT vertex_offset{};
        ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetVertexBuffers(
            0, 1, &vertex_buffer, &vertex_stride, &vertex_offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* view = upload_view_.Get();
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetShaderResources(0, 1, &view);
        context_->PSSetSamplers(0, 1, &sampler);
        ID3D11Buffer* viewport_buffer = viewport_buffer_.Get();
        context_->PSSetConstantBuffers(0, 1, &viewport_buffer);
        context_->Draw(static_cast<UINT>(video_vertices.size()), 0);

        // Unbind the input texture before reusing/replacing it on the next frame.
        ID3D11ShaderResourceView* empty_view = nullptr;
        context_->PSSetShaderResources(0, 1, &empty_view);
        // Seek/close joins the render worker. Never let presentation wait for
        // vblank or a full DXGI queue while that worker is being retired.
        const auto present_hr = swap_chain_->Present(
            0, DXGI_PRESENT_DO_NOT_WAIT);
        if (present_hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            return VideoPresentStatus::Dropped;
        }
        if (FAILED(present_hr)) {
            record_failure(VideoPresenterFailureStage::Present, present_hr);
        }
        return SUCCEEDED(present_hr)
            ? VideoPresentStatus::Presented
            : failed_present(present_hr);
    }

private:
    // Deliberately keep counters after release() so diagnostics describe a
    // complete playback generation, including failed resource rebuilds.
    // All mutations except diagnostics() occur on the render thread.
    void record_failure(
        VideoPresenterFailureStage stage,
        HRESULT hresult) noexcept {
        const auto reason = device_.Get() != nullptr
            ? device_->GetDeviceRemovedReason()
            : S_OK;
        if (device_removed(hresult) || device_removed(reason)) {
            device_loss_events_.fetch_add(1, std::memory_order_relaxed);
        }
        last_hresult_.store(static_cast<std::uint32_t>(hresult),
            std::memory_order_relaxed);
        last_removed_reason_.store(static_cast<std::uint32_t>(reason),
            std::memory_order_relaxed);
        last_stage_.store(stage, std::memory_order_release);
        if (journal_ != nullptr) {
            journal_->record(current_epoch_.load(std::memory_order_relaxed),
                DiagnosticEventKind::DeviceFailure,
                static_cast<std::uint32_t>(stage),
                static_cast<std::uint32_t>(hresult),
                static_cast<std::uint32_t>(reason));
        }

        char line[224]{};
        (void)std::snprintf(
            line, sizeof(line),
            "Lapis.D3D11 | stage=%s | hr=0x%08X | device_removed_reason=0x%08X"
            " | device_creations=%llu | swap_chains=%llu | resizes=%llu\n",
            failure_stage_name(stage),
            static_cast<unsigned>(static_cast<std::uint32_t>(hresult)),
            static_cast<unsigned>(static_cast<std::uint32_t>(reason)),
            static_cast<unsigned long long>(
                device_creations_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                swap_chain_creations_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                swap_chain_resizes_.load(std::memory_order_relaxed)));
        OutputDebugStringA(line);
    }

    [[nodiscard]] VideoOutputSurfaceApplyStatus initialize(HWND hwnd) {
        constexpr D3D_FEATURE_LEVEL levels[]{
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};
        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        auto create = [&](D3D_DRIVER_TYPE driver) {
            return D3D11CreateDevice(
                nullptr, driver, nullptr, flags,
                levels, static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                device_.GetAddressOf(),
                &selected,
                context_.GetAddressOf());
        };

        auto hr = create(D3D_DRIVER_TYPE_HARDWARE);
        if (FAILED(hr)) {
            context_.Reset();
            device_.Reset();
            // The Windows CI environment and some remote desktops have no
            // physical GPU. WARP makes the same D3D11 path usable there.
            hr = create(D3D_DRIVER_TYPE_WARP);
        }
        if (FAILED(hr)) {
            record_failure(VideoPresenterFailureStage::DeviceCreation, hr);
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }
        const auto devices = device_creations_.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (journal_ != nullptr) {
            journal_->record(current_epoch_.load(std::memory_order_relaxed),
                DiagnosticEventKind::DeviceCreated, 0, 0, 0, devices);
        }

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        hr = device_.As(&dxgi_device);
        if (SUCCEEDED(hr)) {
            hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
        }
        if (FAILED(hr)) {
            record_failure(VideoPresenterFailureStage::AdapterDiscovery, hr);
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }

        RECT client{};
        if (!GetClientRect(hwnd, &client)) {
            record_failure(
                VideoPresenterFailureStage::WindowQuery,
                HRESULT_FROM_WIN32(GetLastError()));
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }
        const auto width = static_cast<UINT>(std::max<LONG>(
            1, client.right - client.left));
        const auto height = static_cast<UINT>(std::max<LONG>(
            1, client.bottom - client.top));

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.Stereo = FALSE;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        hr = factory->CreateSwapChainForHwnd(
            device_.Get(), hwnd, &description, nullptr, nullptr,
            swap_chain_.GetAddressOf());
        if (FAILED(hr)) {
            record_failure(VideoPresenterFailureStage::SwapChainCreation, hr);
            return VideoOutputSurfaceApplyStatus::Unavailable;
        }
        const auto chains = swap_chain_creations_.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (journal_ != nullptr) {
            journal_->record(current_epoch_.load(std::memory_order_relaxed),
                DiagnosticEventKind::SwapChainCreated, 0, 0, 0, chains);
        }
        // We own fullscreen policy; DXGI must not steal Alt+Enter.
        (void)factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        hr = create_target();
        if (FAILED(hr)) {
            record_failure(VideoPresenterFailureStage::RenderTargetCreation, hr);
            return VideoOutputSurfaceApplyStatus::Failed;
        }
        back_width_ = width;
        back_height_ = height;
        hr = create_shaders();
        if (FAILED(hr)) {
            record_failure(VideoPresenterFailureStage::ShaderCreation, hr);
            return VideoOutputSurfaceApplyStatus::Failed;
        }

        hwnd_ = hwnd;
        return VideoOutputSurfaceApplyStatus::Ready;
    }

    [[nodiscard]] HRESULT create_target() {
        ComPtr<ID3D11Texture2D> back_buffer;
        auto hr = swap_chain_->GetBuffer(
            0, IID_PPV_ARGS(back_buffer.GetAddressOf()));
        if (FAILED(hr)) {
            return hr;
        }
        return device_->CreateRenderTargetView(
            back_buffer.Get(), nullptr, render_target_.GetAddressOf());
    }

    [[nodiscard]] HRESULT resize(UINT width, UINT height) {
        context_->ClearState();
        render_target_.Reset();
        const auto hr = swap_chain_->ResizeBuffers(
            0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            record_failure(VideoPresenterFailureStage::ResizeBuffers, hr);
            return hr;
        }
        const auto target_hr = create_target();
        if (FAILED(target_hr)) {
            record_failure(
                VideoPresenterFailureStage::RenderTargetCreation, target_hr);
            return target_hr;
        }
        const auto resizes = swap_chain_resizes_.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (journal_ != nullptr) {
            journal_->record(current_epoch_.load(std::memory_order_relaxed),
                DiagnosticEventKind::SwapChainResized, 0, 0, 0, resizes);
        }
        back_width_ = width;
        back_height_ = height;
        return S_OK;
    }

    [[nodiscard]] HRESULT create_shaders() {
        ComPtr<ID3DBlob> vertex_blob;
        ComPtr<ID3DBlob> pixel_blob;
        auto hr = D3DCompile(
            vertex_shader_source, sizeof(vertex_shader_source) - 1,
            nullptr, nullptr, nullptr, "main", "vs_4_0",
            D3DCOMPILE_ENABLE_STRICTNESS, 0,
            vertex_blob.GetAddressOf(), nullptr);
        if (FAILED(hr)) {
            return hr;
        }
        hr = D3DCompile(
            pixel_shader_source, sizeof(pixel_shader_source) - 1,
            nullptr, nullptr, nullptr, "main", "ps_4_0",
            D3DCOMPILE_ENABLE_STRICTNESS, 0,
            pixel_blob.GetAddressOf(), nullptr);
        if (FAILED(hr)) {
            return hr;
        }
        hr = device_->CreateVertexShader(
            vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
            nullptr, vertex_shader_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
        constexpr D3D11_INPUT_ELEMENT_DESC input_elements[]{
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        hr = device_->CreateInputLayout(
            input_elements,
            static_cast<UINT>(std::size(input_elements)),
            vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
            input_layout_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
        D3D11_BUFFER_DESC viewport_buffer_desc{};
        viewport_buffer_desc.ByteWidth = sizeof(float) * 4;
        viewport_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
        viewport_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        viewport_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device_->CreateBuffer(
            &viewport_buffer_desc, nullptr,
            viewport_buffer_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
        D3D11_BUFFER_DESC vertex_buffer_desc{};
        vertex_buffer_desc.ByteWidth =
            static_cast<UINT>(sizeof(video_vertices));
        vertex_buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertex_data{};
        vertex_data.pSysMem = video_vertices.data();
        hr = device_->CreateBuffer(
            &vertex_buffer_desc, &vertex_data,
            vertex_buffer_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
        hr = device_->CreatePixelShader(
            pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
            nullptr, pixel_shader_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        return device_->CreateSamplerState(
            &sampler_desc, sampler_.GetAddressOf());
    }

    [[nodiscard]] HRESULT ensure_upload(
        std::uint32_t width, std::uint32_t height) {
        if (width == upload_width_ && height == upload_height_ &&
            upload_texture_.Get() != nullptr) {
            return S_OK;
        }
        upload_view_.Reset();
        upload_texture_.Reset();
        upload_width_ = 0;
        upload_height_ = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        auto hr = device_->CreateTexture2D(
            &desc, nullptr, upload_texture_.GetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }
        hr = device_->CreateShaderResourceView(
            upload_texture_.Get(), nullptr, upload_view_.GetAddressOf());
        if (FAILED(hr)) {
            upload_texture_.Reset();
            return hr;
        }
        upload_width_ = width;
        upload_height_ = height;
        return S_OK;
    }

    std::shared_ptr<DiagnosticEventJournal> journal_;
    std::atomic<std::uint64_t> current_epoch_{};
    std::atomic<std::uint64_t> device_creations_{};
    std::atomic<std::uint64_t> swap_chain_creations_{};
    std::atomic<std::uint64_t> swap_chain_resizes_{};
    std::atomic<std::uint64_t> device_loss_events_{};
    std::atomic<std::uint32_t> last_hresult_{};
    std::atomic<std::uint32_t> last_removed_reason_{};
    std::atomic<VideoPresenterFailureStage> last_stage_{
        VideoPresenterFailureStage::None
    };

    HWND hwnd_{};
    UINT back_width_{};
    UINT back_height_{};
    std::uint32_t upload_width_{};
    std::uint32_t upload_height_{};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11InputLayout> input_layout_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> viewport_buffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Texture2D> upload_texture_;
    ComPtr<ID3D11ShaderResourceView> upload_view_;
};

D3D11VideoPresenter::D3D11VideoPresenter() : impl_(std::make_unique<Impl>()) {}

D3D11VideoPresenter::~D3D11VideoPresenter() = default;

VideoOutputSurfaceApplyStatus D3D11VideoPresenter::set_output_surface(
    const VideoOutputSurfaceDescriptor& surface) {
    return impl_->bind(surface);
}

VideoPresenterBeginStatus D3D11VideoPresenter::begin(PlaybackEpoch epoch) {
    impl_->set_epoch(epoch.value());
    // Binding and device creation are handled by set_output_surface on this
    // same render thread. An unbound session deliberately remains headless.
    return VideoPresenterBeginStatus::Ready;
}

VideoPresentStatus D3D11VideoPresenter::present(
    const VideoFrame& frame, const VideoPresentContext&) {
    return impl_->draw(frame);
}

void D3D11VideoPresenter::set_diagnostics_journal(
    std::shared_ptr<DiagnosticEventJournal> journal) noexcept {
    impl_->set_journal(std::move(journal));
}

VideoPresenterDiagnostics D3D11VideoPresenter::diagnostics() const noexcept {
    return impl_->diagnostics();
}

void D3D11VideoPresenter::end() noexcept {
    impl_->release();
}

} // namespace lapis::media::d3d11
