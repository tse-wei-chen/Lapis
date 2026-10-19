#pragma once

#if !defined(_WIN32)
#error WASAPI event waiting is Windows-only.
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "audio/audio_output.hpp"

#include <stop_token>
#include <utility>

namespace lapis::media::wasapi::detail {

// An invalidated endpoint can stop signaling its event altogether. The wait
// therefore polls the device state at a bounded interval while remaining
// otherwise event driven. This helper is intentionally internal; a fake probe
// can exercise the exact production wait behavior without touching the OS's
// default audio device or offering a fault-injection switch to users.
enum class WritableProbeResult {
    Pending,
    Writable,
    DeviceLost,
    Faulted
};

template <typename Probe>
[[nodiscard]] AudioOutputWaitStatus wait_for_writable_or_device_loss(
    HANDLE audio_event,
    HANDLE stop_event,
    std::stop_token stop_token,
    Probe&& probe) {
    if (audio_event == nullptr || stop_event == nullptr) {
        return AudioOutputWaitStatus::Faulted;
    }
    if (stop_token.stop_requested()) {
        return AudioOutputWaitStatus::Stopped;
    }
    if (ResetEvent(stop_event) == FALSE) {
        return AudioOutputWaitStatus::Faulted;
    }

    // Stop is still interrupt-driven, not limited by the polling interval.
    // AudioEvent/StopEvent are owned by the calling render thread; the
    // callback lifetime cannot outlive the function's stack frame.
    std::stop_callback callback(stop_token, [stop_event]() noexcept {
        (void)SetEvent(stop_event);
    });
    const HANDLE events[]{audio_event, stop_event};
    constexpr DWORD device_probe_interval_ms = 100;

    for (;;) {
        if (stop_token.stop_requested()) {
            return AudioOutputWaitStatus::Stopped;
        }
        const auto result = WaitForMultipleObjects(
            2, events, FALSE, device_probe_interval_ms);
        if (stop_token.stop_requested() || result == WAIT_OBJECT_0 + 1) {
            return AudioOutputWaitStatus::Stopped;
        }
        if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) {
            return AudioOutputWaitStatus::Faulted;
        }

        // Query device state on both real audio events and timeouts.
        // Do not report Ready on a timeout when no buffer space exists.
        switch (std::forward<Probe>(probe)()) {
            case WritableProbeResult::Writable:
                return AudioOutputWaitStatus::Ready;
            case WritableProbeResult::DeviceLost:
                return AudioOutputWaitStatus::DeviceLost;
            case WritableProbeResult::Faulted:
                return AudioOutputWaitStatus::Faulted;
            case WritableProbeResult::Pending:
                break;
        }
    }
}

} // namespace lapis::media::wasapi::detail
