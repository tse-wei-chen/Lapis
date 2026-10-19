#include "lapis/media_runtime.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

lapis_playback_session_id test_session_id() {
    lapis_playback_session_id id{};
    for (std::size_t index = 0; index < 16; ++index) {
        id.bytes[index] = static_cast<uint8_t>(index + 1);
    }
    return id;
}

bool same_session_id(
    const lapis_playback_session_id& left,
    const lapis_playback_session_id& right) {
    for (std::size_t index = 0; index < 16; ++index) {
        if (left.bytes[index] != right.bytes[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    CHECK(lapis_runtime_get_abi_version() == LAPIS_MEDIA_RUNTIME_ABI_VERSION);
    CHECK(lapis_runtime_get_diagnostics_api_version() ==
        LAPIS_DIAGNOSTICS_API_VERSION);
    CHECK(sizeof(lapis_diagnostic_event) == 48);

    lapis_runtime* runtime = nullptr;
    CHECK(lapis_runtime_create(&runtime) == LAPIS_OK);
    CHECK(runtime != nullptr);

    const auto session_id = test_session_id();
    lapis_session* session = nullptr;
    CHECK(lapis_session_create(runtime, session_id, &session) == LAPIS_OK);
    CHECK(session != nullptr);

    lapis_diagnostic_event events[LAPIS_DIAGNOSTIC_RING_CAPACITY]{};
    uint32_t event_count{};
    uint64_t next_sequence{};
    uint64_t overwritten{};
    CHECK(lapis_session_read_diagnostic_events(
        session, 0, events, LAPIS_DIAGNOSTIC_RING_CAPACITY,
        &event_count, &next_sequence, &overwritten) == LAPIS_OK);
    CHECK(event_count == 0);
    CHECK(next_sequence == 0);
    CHECK(overwritten == 0);
    CHECK(lapis_session_read_diagnostic_events(
        session, 0, nullptr, 1,
        &event_count, &next_sequence, &overwritten) == LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_read_diagnostic_events(
        session, 0, events, LAPIS_DIAGNOSTIC_RING_CAPACITY + 1,
        &event_count, &next_sequence, &overwritten) == LAPIS_E_INVALID_ARGUMENT);

    lapis_video_output_surface_binding binding{};
    const lapis_video_output_surface detached{LAPIS_VIDEO_OUTPUT_SURFACE_NONE, 0};
    const lapis_video_output_surface window{LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW, 0x1234};
    CHECK(lapis_session_set_video_output_surface(session, detached, {0}, nullptr) == LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_set_video_output_surface(session, {LAPIS_VIDEO_OUTPUT_SURFACE_NONE, 1}, {0}, &binding) == LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_set_video_output_surface(session, {LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW, 0}, {0}, &binding) == LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_set_video_output_surface(session, {static_cast<lapis_video_output_surface_kind>(99), 1}, {0}, &binding) == LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_set_video_output_surface(session, window, {1}, &binding) == LAPIS_E_STALE_EPOCH);
    CHECK(lapis_session_set_video_output_surface(session, window, {0}, &binding) == LAPIS_OK);
    CHECK(binding == LAPIS_OUTPUT_SURFACE_DEFERRED);
    CHECK(lapis_session_set_video_output_surface(session, detached, {0}, &binding) == LAPIS_OK);
    CHECK(binding == LAPIS_OUTPUT_SURFACE_DEFERRED);

    lapis_playback_snapshot snapshot{};
    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(same_session_id(snapshot.session_id, session_id));
    CHECK(snapshot.transport == LAPIS_TRANSPORT_CLOSED);
    CHECK(snapshot.epoch.value == 0);

    const lapis_media_locator locator{LAPIS_LOCATOR_FILE, LAPIS_TEST_MEDIA_PATH};
    lapis_open_result open_result{};
    CHECK(lapis_session_open(session, &locator, {1}, &open_result) == LAPIS_OK);

    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_READY);
    CHECK(snapshot.epoch.value == 1);
    CHECK(lapis_session_set_video_output_surface(session, detached, {0}, &binding) == LAPIS_E_STALE_EPOCH);
    CHECK(lapis_session_set_video_output_surface(session, detached, {1}, &binding) == LAPIS_OK);
    CHECK(binding == LAPIS_OUTPUT_SURFACE_DEFERRED);

    // Seeking while READY prepares a stopped PlaybackExecution timeline. Play
    // must use that same epoch; beginning presentation is not a discontinuity.
    CHECK(lapis_session_seek(session, {0}, {2}) == LAPIS_OK);
    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_READY);
    CHECK(snapshot.epoch.value == 2);
    CHECK(snapshot.position.nanoseconds == 0);
    CHECK(lapis_session_set_video_output_surface(session, detached, {1}, &binding) == LAPIS_E_STALE_EPOCH);
    CHECK(lapis_session_set_video_output_surface(session, detached, {2}, &binding) == LAPIS_OK);


    CHECK(lapis_session_play(session, {2}) == LAPIS_OK);
    CHECK(lapis_session_pause(session, {2}) == LAPIS_OK);

    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_PAUSED);
    CHECK(snapshot.epoch.value == 2);
    CHECK(snapshot.position.nanoseconds >= 0);

    CHECK(lapis_session_play(session, {2}) == LAPIS_OK);
    CHECK(lapis_session_close(session, {3}) == LAPIS_OK);

    // A command captured from the previous timeline is rejected before state checks.
    CHECK(lapis_session_pause(session, {2}) == LAPIS_E_STALE_EPOCH);

    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_CLOSED);
    CHECK(snapshot.epoch.value == 3);
    CHECK(lapis_session_set_video_output_surface(session, window, {3}, &binding) == LAPIS_OK);
    CHECK(binding == LAPIS_OUTPUT_SURFACE_DEFERRED);

    CHECK(snapshot.position.nanoseconds == 0);

    // Natural Ended/Faulted publication belongs to the serialized command path.
    // Do not call get_snapshot while the tiny fixture drains: pause() itself must
    // refresh the execution terminal state before validating PLAYING, rather than
    // depending on a previous snapshot read to mutate session transport.
    open_result = {};
    CHECK(lapis_session_open(session, &locator, {4}, &open_result) == LAPIS_OK);
    // Pre-open HWND intent is preserved across the open boundary; the headless
    // composition has no HWND presenter yet, so clear it before Play.
    CHECK(lapis_session_set_video_output_surface(session, detached, {4}, &binding) == LAPIS_OK);
    CHECK(binding == LAPIS_OUTPUT_SURFACE_DEFERRED);
    CHECK(lapis_session_play(session, {4}) == LAPIS_OK);
    std::this_thread::sleep_for(250ms);
    CHECK(lapis_session_pause(session, {4}) == LAPIS_E_INVALID_STATE);

    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_ENDED);
    CHECK(snapshot.epoch.value == 4);

    CHECK(lapis_session_close(session, {5}) == LAPIS_OK);

    #if defined(_WIN32)
    // End-to-end C ABI integration: composition must use a real HWND-capable
    // D3D11 presenter rather than silently dropping video in Headless mode.
    HWND test_window = CreateWindowExW(
        0, L"STATIC", L"Lapis runtime D3D11 integration",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 320, 240,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(test_window != nullptr);
    const lapis_video_output_surface actual_window{
        LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW,
        reinterpret_cast<uintptr_t>(test_window)
    };
    CHECK(lapis_session_set_video_output_surface(session, actual_window, {5}, &binding) == LAPIS_OK);
    CHECK(binding == LAPIS_OUTPUT_SURFACE_DEFERRED);
    CHECK(lapis_session_open(session, &locator, {6}, &open_result) == LAPIS_OK);
    CHECK(lapis_session_play(session, {6}) == LAPIS_OK);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
        if (snapshot.transport == LAPIS_TRANSPORT_ENDED ||
            snapshot.transport == LAPIS_TRANSPORT_FAULTED) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_ENDED);
    CHECK(lapis_session_close(session, {7}) == LAPIS_OK);
    CHECK(DestroyWindow(test_window) != FALSE);
    #endif

    lapis_session_destroy(session);
    lapis_runtime_destroy(runtime);
    return 0;
}
