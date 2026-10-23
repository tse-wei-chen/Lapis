#include "video/d3d11/d3d11_video_presenter.hpp"

#if !defined(_WIN32)
#error D3D11 presenter tests are Windows-only.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

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

// Rendering deliberately uses DXGI_PRESENT_DO_NOT_WAIT. After a burst of
// frames or a swap-chain resize, a full presentation queue can legitimately
// drop a frame; it must never be mistaken for a device failure.
// Require eventual progress with a deadline, and fail immediately on any
// non-transient status so the test cannot conceal genuine GPU regressions.
[[nodiscard]] bool present_eventually(
    lapis::media::d3d11::D3D11VideoPresenter& presenter,
    const lapis::media::VideoFrame& frame,
    const lapis::media::VideoPresentContext& context) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    lapis::media::VideoPresentStatus last{};
    do {
        last = presenter.present(frame, context);
        if (last == lapis::media::VideoPresentStatus::Presented) {
            return true;
        }
        if (last != lapis::media::VideoPresentStatus::Dropped) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);

    const auto diagnostic = presenter.diagnostics();
    std::cerr << "D3D11 presenter stalled/failed: status="
              << static_cast<int>(last)
              << " stage=" << static_cast<int>(diagnostic.last_failure_stage)
              << " hresult=0x" << std::hex << diagnostic.last_hresult
              << " device_removed=0x" << diagnostic.last_device_removed_reason
              << std::dec << '\n';
    return false;
}

} // namespace

int main() {
    using namespace lapis::media;
    using lapis::media::d3d11::D3D11VideoPresenter;

    D3D11VideoPresenter presenter;
    auto journal = std::make_shared<DiagnosticEventJournal>();
    presenter.set_diagnostics_journal(journal);
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
    CHECK(present_eventually(presenter, good, context));

    CHECK(presenter.set_output_surface(
        {VideoOutputSurfaceKind::Win32Window, 0}) ==
        VideoOutputSurfaceApplyStatus::Failed);
    CHECK(presenter.set_output_surface(
        {VideoOutputSurfaceKind::Win32Window, 0x1234}) ==
        VideoOutputSurfaceApplyStatus::Unavailable);
    const auto initial_diagnostics = presenter.diagnostics();
    CHECK(initial_diagnostics.last_failure_stage ==
        VideoPresenterFailureStage::WindowQuery);
    CHECK(initial_diagnostics.last_hresult != 0);
    CHECK(initial_diagnostics.last_device_removed_reason == 0);
    CHECK(initial_diagnostics.swap_chain_creations == 0);
    DiagnosticEvent events[DiagnosticEventJournal::capacity]{};
    std::uint64_t next_sequence{};
    std::uint64_t dropped{};
    const auto recorded = journal->read_after(0, events,
        DiagnosticEventJournal::capacity, next_sequence, dropped);
    CHECK(recorded >= 1);
    CHECK(events[recorded - 1].kind == DiagnosticEventKind::DeviceFailure);
    CHECK(events[recorded - 1].failure_stage ==
        static_cast<std::uint32_t>(VideoPresenterFailureStage::WindowQuery));
    CHECK(events[recorded - 1].hresult != 0);

    HWND first = make_window();
    CHECK(first != nullptr);
    CHECK(presenter.set_output_surface(for_window(first)) ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(presenter.present(unsupported, context) ==
        VideoPresentStatus::UnsupportedSurface);
    CHECK(present_eventually(presenter, good, context));
    CHECK(presenter.diagnostics().device_creations == 1);
    CHECK(presenter.diagnostics().swap_chain_creations == 1);

    const auto present_batch_started = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 180; ++frame) {
        const auto status = presenter.present(good, context);
        CHECK(status == VideoPresentStatus::Presented ||
            status == VideoPresentStatus::Dropped);
    }
    CHECK(std::chrono::steady_clock::now() - present_batch_started <
        std::chrono::seconds(2));

    CHECK(MoveWindow(first, 100, 100, 500, 220, TRUE) != FALSE);
    CHECK(present_eventually(presenter, good, context));
    CHECK(presenter.diagnostics().swap_chain_resizes >= 1);

    HWND replacement = make_window();
    CHECK(replacement != nullptr);
    CHECK(presenter.set_output_surface(for_window(replacement)) ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(DestroyWindow(first) != FALSE);
    CHECK(present_eventually(presenter, good, context));
    CHECK(presenter.diagnostics().swap_chain_creations == 2);
    CHECK(presenter.diagnostics().device_creations == 2);

    CHECK(presenter.set_output_surface(VideoOutputSurfaceDescriptor{}) ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(DestroyWindow(replacement) != FALSE);
    CHECK(present_eventually(presenter, good, context));
    presenter.end();
    // Explicitly releasing COM resources does not erase fault evidence.
    CHECK(presenter.diagnostics().swap_chain_creations == 2);
    CHECK(presenter.diagnostics().last_failure_stage ==
        VideoPresenterFailureStage::WindowQuery);
    return 0;
}
