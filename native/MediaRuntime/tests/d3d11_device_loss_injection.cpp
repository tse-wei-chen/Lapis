#include "execution/playback_execution.hpp"
#include "process/ffmpeg/ffmpeg_video_frame_converter.hpp"
#include "session/playback_media_session.hpp"
#include "video/d3d11/d3d11_video_presenter.hpp"

#if !defined(_WIN32)
#error The live D3D11 DeviceLost-injection acceptance test is Windows-only.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

using namespace lapis::media;
using namespace std::chrono_literals;

// The injector exists only in the test binary. The underlying present(),
// D3D11 resource release, swap-chain creation and post-recovery Present
// still use the actual production presenter and real Win32 output window.
// No production DLL export, environment variable or driver reset is needed.
struct FaultProbe final {
    std::atomic<std::uint32_t> successful_presents{};
    std::atomic<std::uint32_t> injected_device_losses{};
    std::atomic<std::uint32_t> native_binds{};
    std::atomic<std::uint32_t> native_detaches{};
    std::atomic<std::uint32_t> end_calls{};
};

class FaultInjectingD3D11Presenter final : public IVideoPresenter {
public:
    FaultInjectingD3D11Presenter(
        std::shared_ptr<FaultProbe> probe,
        std::uint32_t inject_after_successes,
        std::uint32_t loss_budget)
        : probe_(std::move(probe)),
          inject_after_successes_(inject_after_successes),
          loss_budget_(loss_budget) {}

    VideoOutputSurfaceApplyStatus set_output_surface(
        const VideoOutputSurfaceDescriptor& surface) override {
        const auto status = real_.set_output_surface(surface);
        if (status == VideoOutputSurfaceApplyStatus::Ready) {
            if (surface.kind == VideoOutputSurfaceKind::None) {
                probe_->native_detaches.fetch_add(1, std::memory_order_relaxed);
            } else {
                probe_->native_binds.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return status;
    }

    VideoPresenterBeginStatus begin(PlaybackEpoch epoch) override {
        return real_.begin(epoch);
    }

    VideoPresentStatus present(
        const VideoFrame& frame,
        const VideoPresentContext& context) override {
        if (probe_->successful_presents.load(std::memory_order_relaxed) >=
                inject_after_successes_ &&
            probe_->injected_device_losses.load(std::memory_order_relaxed) <
                loss_budget_) {
            probe_->injected_device_losses.fetch_add(
                1, std::memory_order_relaxed);
            return VideoPresentStatus::DeviceLost;
        }
        const auto result = real_.present(frame, context);
        if (result == VideoPresentStatus::Presented) {
            probe_->successful_presents.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    void set_diagnostics_journal(
        std::shared_ptr<DiagnosticEventJournal> journal) noexcept override {
        real_.set_diagnostics_journal(std::move(journal));
    }

    VideoPresenterDiagnostics diagnostics() const noexcept override {
        return real_.diagnostics();
    }

    void end() noexcept override {
        probe_->end_calls.fetch_add(1, std::memory_order_relaxed);
        real_.end();
    }

private:
    d3d11::D3D11VideoPresenter real_;
    std::shared_ptr<FaultProbe> probe_;
    std::uint32_t inject_after_successes_{};
    std::uint32_t loss_budget_{};
};

class TestWindow final {
public:
    TestWindow() {
        hwnd_ = CreateWindowExW(
            0, L"STATIC", L"Lapis GPU fault injection",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 480, 360, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
    }

    ~TestWindow() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
        }
    }

    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept { return hwnd_; }

private:
    HWND hwnd_{};
};

void pump_messages() noexcept {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

[[nodiscard]] bool wait_terminal(
    PlaybackExecution& execution,
    PlaybackExecutionSnapshot& snapshot,
    std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        pump_messages();
        snapshot = execution.snapshot();
        if (snapshot.state == PlaybackExecutionState::Ended ||
            snapshot.state == PlaybackExecutionState::Faulted) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    snapshot = execution.snapshot();
    return false;
}

[[nodiscard]] std::uint32_t count_events(
    const DiagnosticEvent* entries,
    std::uint32_t size,
    DiagnosticEventKind kind,
    std::uint64_t epoch) {
    std::uint32_t count{};
    for (std::uint32_t i = 0; i < size; ++i) {
        if (entries[i].kind == kind && entries[i].epoch == epoch) {
            ++count;
        }
    }
    return count;
}

enum class FaultScenario { Recovered, RetryExhausted, NoPresentBeforeEof };

[[nodiscard]] int run_case(
    const char* media_path,
    FaultScenario scenario,
    std::uint32_t inject_after_successes,
    std::uint32_t inject_count) {
    TestWindow window;
    CHECK(window.handle() != nullptr);
    PlaybackMediaSession media;
    const MediaLocator locator{MediaLocatorKind::File, media_path};
    CHECK(media.open(locator).ok());
    CHECK(media.has_video());
    auto probe = std::make_shared<FaultProbe>();
    auto journal = std::make_shared<DiagnosticEventJournal>();

    {
        PlaybackExecution execution(
            media,
            std::make_unique<ffmpeg::FfmpegVideoFrameConverter>(),
            std::make_unique<FaultInjectingD3D11Presenter>(
                probe, inject_after_successes, inject_count),
            PlaybackExecutionConfig{},
            journal);
        const VideoOutputSurfaceDescriptor output{
            VideoOutputSurfaceKind::Win32Window,
            reinterpret_cast<std::uintptr_t>(window.handle())
        };
        CHECK(execution.set_video_output_surface(output) ==
            VideoOutputSurfaceControlStatus::Deferred);
        CHECK(execution.start(PlaybackEpoch{41}, MediaTime{}).ok());

        PlaybackExecutionSnapshot snapshot{};
        CHECK(wait_terminal(execution, snapshot, 15s));
        const auto stats = snapshot.rendering;
        const auto diagnostics = snapshot.presenter_diagnostics;

        DiagnosticEvent events[DiagnosticEventJournal::capacity]{};
        std::uint64_t last_sequence{};
        std::uint64_t dropped{};
        const auto count = journal->read_after(
            0, events, DiagnosticEventJournal::capacity,
            last_sequence, dropped);
        CHECK(count > 0);
        CHECK(dropped == 0);
        CHECK(last_sequence == events[count - 1].sequence);
        CHECK(probe->native_binds.load(std::memory_order_relaxed) ==
            diagnostics.swap_chain_creations);
        CHECK(diagnostics.device_creations ==
            diagnostics.swap_chain_creations);

        if (scenario == FaultScenario::Recovered) {
            // At least one frame must have reached real D3D before injecting
            // DeviceLost, followed by a completely new device and swap-chain
            // and a successful real post-recovery Present (not just Ready).
            CHECK(snapshot.state == PlaybackExecutionState::Ended);
            CHECK(stats.recovery_attempts == 1);
            CHECK(stats.recovery_bind_successes == 1);
            CHECK(stats.recovered_presents == 1);
            CHECK(stats.presented_frames >= 2);
            CHECK(probe->successful_presents.load() >= 2);
            CHECK(probe->injected_device_losses.load() == 1);
            CHECK(diagnostics.device_creations >= 2);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryAttempted, 41) == 1);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryBindSucceeded, 41) == 1);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryPresented, 41) == 1);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryTerminated, 41) == 0);
        } else if (scenario == FaultScenario::RetryExhausted) {
            // Two rebinding attempts are permitted; the third synthetic
            // DeviceLost must surface a terminal, observable error.
            CHECK(snapshot.state == PlaybackExecutionState::Faulted);
            CHECK(snapshot.render_state ==
                VideoRenderWorkerState::PresenterUnavailable);
            CHECK(stats.recovery_attempts == 2);
            CHECK(stats.recovery_bind_successes == 2);
            CHECK(stats.recovered_presents == 0);
            CHECK(probe->injected_device_losses.load() == 3);
            CHECK(diagnostics.device_creations >= 3);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryAttempted, 41) == 2);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryBindSucceeded, 41) == 2);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryTerminated, 41) == 1);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryPresented, 41) == 0);
        } else {
            // The last scheduled frame triggers DeviceLost and the device
            // can be rebuilt, but no subsequent Presented frame exists.
            // Clean EOF would incorrectly conceal failed recovery here.
            CHECK(snapshot.state == PlaybackExecutionState::Faulted);
            CHECK(snapshot.render_state ==
                VideoRenderWorkerState::PresenterUnavailable);
            CHECK(stats.recovery_attempts == 1);
            CHECK(stats.recovery_bind_successes == 1);
            CHECK(stats.recovered_presents == 0);
            CHECK(probe->injected_device_losses.load() == 1);
            CHECK(diagnostics.device_creations >= 2);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryIncompleteAtEof, 41) == 1);
            CHECK(count_events(events, count,
                DiagnosticEventKind::RecoveryPresented, 41) == 0);
        }

        // No D3D reference is allowed to outlive this HWND. The renderer
        // must synchronously stop and drop the device on Close.
        CHECK(execution.close(PlaybackEpoch{42}).ok());
        CHECK(execution.snapshot().state == PlaybackExecutionState::Stopped);
        CHECK(probe->native_detaches.load(std::memory_order_relaxed) >= 1);
        CHECK(probe->end_calls.load(std::memory_order_relaxed) >= 1);
    }
    return 0;
}

} // namespace

int main() {
    CHECK(run_case(
        LAPIS_TEST_CHURN_MEDIA_PATH,
        FaultScenario::Recovered, 1, 1) == 0);
    CHECK(run_case(
        LAPIS_TEST_CHURN_MEDIA_PATH,
        FaultScenario::RetryExhausted, 1, 3) == 0);
    CHECK(run_case(
        LAPIS_TEST_TWO_FRAME_MEDIA_PATH,
        FaultScenario::NoPresentBeforeEof, 1, 1) == 0);
    return 0;
}
