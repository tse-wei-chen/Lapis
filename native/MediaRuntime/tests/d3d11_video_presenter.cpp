#include "video/d3d11/d3d11_video_presenter.hpp"

#if !defined(_WIN32)
#error D3D11 presenter tests are Windows-only.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

class BgraTestSurface final : public lapis::media::ISystemMemoryVideoSurface {
public:
    explicit BgraTestSurface(lapis::media::VideoPixelFormat format)
        : format_(format) {}

    lapis::media::VideoPixelFormat pixel_format() const noexcept override {
        return format_;
    }

    std::uint32_t width() const noexcept override { return 2; }
    std::uint32_t height() const noexcept override { return 2; }
    std::uint32_t plane_count() const noexcept override { return 1; }

    lapis::media::VideoPlaneView plane(std::uint32_t index) const noexcept override {
        if (index != 0) {
            return {};
        }
        return {reinterpret_cast<const std::byte*>(pixels_.data()), 8};
    }

private:
    lapis::media::VideoPixelFormat format_;
    // Two rows, each with BGRA pixels: red/green and blue/white.
    std::array<std::uint8_t, 16> pixels_{{
        0, 0, 255, 255, 0, 255, 0, 255,
        255, 0, 0, 255, 255, 255, 255, 255
    }};
};

[[nodiscard]] HWND make_window() {
    return CreateWindowExW(
        0, L"STATIC", L"Lapis D3D11 smoke",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 320, 240,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

[[nodiscard]] lapis::media::VideoOutputSurfaceDescriptor for_window(HWND hwnd) {
    return {
        lapis::media::VideoOutputSurfaceKind::Win32Window,
        reinterpret_cast<std::uintptr_t>(hwnd)
    };
}

} // namespace

int main() {
    using namespace lapis::media;
    using lapis::media::d3d11::D3D11VideoPresenter;

    D3D11VideoPresenter presenter;
    const auto good = VideoFrame{
        std::make_shared<BgraTestSurface>(VideoPixelFormat::Bgra8),
        MediaTime{}, std::nullopt
    };
    const auto unsupported = VideoFrame{
        std::make_shared<BgraTestSurface>(VideoPixelFormat::Nv12),
        MediaTime{}, std::nullopt
    };
    const VideoPresentContext context{PlaybackEpoch{7}};

    CHECK(presenter.begin(PlaybackEpoch{7}) == VideoPresenterBeginStatus::Ready);
    CHECK(presenter.present(good, context) == VideoPresentStatus::Presented);

    CHECK(presenter.set_output_surface(
        {VideoOutputSurfaceKind::Win32Window, 0}) ==
        VideoOutputSurfaceApplyStatus::Failed);
    CHECK(presenter.set_output_surface(
        {VideoOutputSurfaceKind::Win32Window, 0x1234}) ==
        VideoOutputSurfaceApplyStatus::Unavailable);

    HWND first = make_window();
    CHECK(first != nullptr);
    CHECK(presenter.set_output_surface(for_window(first)) ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(presenter.present(unsupported, context) ==
        VideoPresentStatus::UnsupportedSurface);
    CHECK(presenter.present(good, context) == VideoPresentStatus::Presented);

    CHECK(MoveWindow(first, 100, 100, 500, 220, TRUE) != FALSE);
    CHECK(presenter.present(good, context) == VideoPresentStatus::Presented);

    HWND replacement = make_window();
    CHECK(replacement != nullptr);
    CHECK(presenter.set_output_surface(for_window(replacement)) ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(DestroyWindow(first) != FALSE);
    CHECK(presenter.present(good, context) == VideoPresentStatus::Presented);

    CHECK(presenter.set_output_surface(VideoOutputSurfaceDescriptor{}) ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(DestroyWindow(replacement) != FALSE);
    CHECK(presenter.present(good, context) == VideoPresentStatus::Presented);
    presenter.end();
    return 0;
}
