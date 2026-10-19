#include "audio/wasapi/wasapi_event_wait.hpp"

#if !defined(_WIN32)
#error WASAPI event-wait tests run only on Windows.
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stop_token>
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
using namespace lapis::media::wasapi::detail;
using namespace std::chrono_literals;

class AudioEvents final {
public:
    AudioEvents() {
        audio_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~AudioEvents() {
        if (audio_ != nullptr) {
            CloseHandle(audio_);
        }
        if (stop_ != nullptr) {
            CloseHandle(stop_);
        }
    }

    AudioEvents(const AudioEvents&) = delete;
    AudioEvents& operator=(const AudioEvents&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return audio_ != nullptr && stop_ != nullptr;
    }
    [[nodiscard]] HANDLE audio() const noexcept { return audio_; }
    [[nodiscard]] HANDLE stop() const noexcept { return stop_; }

private:
    HANDLE audio_{};
    HANDLE stop_{};
};

} // namespace

int main() {
    // No device callback/event is delivered after unplug. The old INFINITE
    // wait would hang indefinitely; the polling probe detects invalidation.
    {
        AudioEvents events;
        CHECK(events.valid());
        std::uint32_t probes{};
        const auto started = std::chrono::steady_clock::now();
        const auto outcome = wait_for_writable_or_device_loss(
            events.audio(), events.stop(), std::stop_token{}, [&] {
                ++probes;
                return WritableProbeResult::DeviceLost;
            });
        CHECK(outcome == AudioOutputWaitStatus::DeviceLost);
        CHECK(probes == 1);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(elapsed >= 60ms);
        CHECK(elapsed < 2s);
    }

    // Timeout is also a readiness fallback if a driver fails to notify, but
    // only when actual device padding indicates writable space.
    {
        AudioEvents events;
        CHECK(events.valid());
        std::uint32_t probes{};
        const auto outcome = wait_for_writable_or_device_loss(
            events.audio(), events.stop(), std::stop_token{}, [&] {
                ++probes;
                return probes == 1
                    ? WritableProbeResult::Pending
                    : WritableProbeResult::Writable;
            });
        CHECK(outcome == AudioOutputWaitStatus::Ready);
        CHECK(probes == 2);
    }

    // A spurious event with full padding must NOT be interpreted as Ready.
    {
        AudioEvents events;
        CHECK(events.valid());
        CHECK(SetEvent(events.audio()) != FALSE);
        std::uint32_t probes{};
        const auto outcome = wait_for_writable_or_device_loss(
            events.audio(), events.stop(), std::stop_token{}, [&] {
                ++probes;
                return probes == 1
                    ? WritableProbeResult::Pending
                    : WritableProbeResult::Writable;
            });
        CHECK(outcome == AudioOutputWaitStatus::Ready);
        CHECK(probes == 2);
    }

    // A non-device HRESULT maps to Faulted, not an ambiguous timeout.
    {
        AudioEvents events;
        CHECK(events.valid());
        const auto outcome = wait_for_writable_or_device_loss(
            events.audio(), events.stop(), std::stop_token{}, [] {
                return WritableProbeResult::Faulted;
            });
        CHECK(outcome == AudioOutputWaitStatus::Faulted);
    }

    // Unlike polling, a Stop/Seek must wake the Windows wait immediately
    // through the stop-event callback. No fake device probe is invoked.
    {
        AudioEvents events;
        CHECK(events.valid());
        std::stop_source source;
        std::atomic<AudioOutputWaitStatus> result{
            AudioOutputWaitStatus::Faulted
        };
        std::atomic<std::uint32_t> probes{};
        std::jthread waiter([&] {
            result.store(wait_for_writable_or_device_loss(
                events.audio(), events.stop(), source.get_token(), [&] {
                    probes.fetch_add(1, std::memory_order_relaxed);
                    return WritableProbeResult::Pending;
                }), std::memory_order_release);
        });
        std::this_thread::sleep_for(15ms);
        const auto stop_started = std::chrono::steady_clock::now();
        CHECK(source.request_stop());
        waiter.join();
        CHECK(result.load(std::memory_order_acquire) ==
            AudioOutputWaitStatus::Stopped);
        CHECK(std::chrono::steady_clock::now() - stop_started < 2s);
    }

    // Null kernel handles do not trigger an uncontrolled WaitForMultipleObjects.
    CHECK(wait_for_writable_or_device_loss(
        nullptr, nullptr, std::stop_token{}, [] {
            return WritableProbeResult::Writable;
        }) == AudioOutputWaitStatus::Faulted);

    return 0;
}
