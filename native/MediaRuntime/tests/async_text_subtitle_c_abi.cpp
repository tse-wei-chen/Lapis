#include <lapis/media_runtime.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {
using namespace std::chrono_literals;

bool await_ready(lapis_session* session, lapis_playback_epoch epoch,
                 std::uint32_t& total) {
    for (int i = 0; i < 1000; ++i) {
        lapis_text_subtitle_load_state state{};
        std::uint32_t count{}, bytes{};
        const auto status = lapis_session_read_text_subtitle_cues(
            session, epoch, 0, nullptr, 0, nullptr, 0,
            &count, &total, &bytes, &state);
        if (status != LAPIS_OK) return false;
        if (state == LAPIS_TEXT_SUBTITLE_READY) return true;
        if (state != LAPIS_TEXT_SUBTITLE_LOADING) return false;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
}

int main() {
    static_assert(sizeof(lapis_text_subtitle_cue_info) == 24);
    static_assert(offsetof(lapis_text_subtitle_cue_info, text_offset) == 16);
    CHECK(lapis_runtime_get_text_subtitle_cues_api_version() ==
          LAPIS_TEXT_SUBTITLE_CUES_API_VERSION);
    CHECK(lapis_runtime_get_text_subtitle_cancel_api_version() ==
          LAPIS_TEXT_SUBTITLE_CANCEL_API_VERSION);

    lapis_runtime* runtime = nullptr;
    CHECK(lapis_runtime_create(&runtime) == LAPIS_OK);
    lapis_session* session = nullptr;
    lapis_playback_session_id id{};
    id.bytes[0] = 5;
    CHECK(lapis_session_create(runtime, id, &session) == LAPIS_OK);

    lapis_playback_epoch first{1};
    lapis_media_locator media{LAPIS_LOCATOR_FILE, LAPIS_TEST_SUBTITLE_MEDIA_PATH};
    lapis_open_result open{};
    CHECK(lapis_session_open(session, &media, first, &open) == LAPIS_OK);

    // Stream 0 is video; only stream 1 can launch the text decoder.
    CHECK(lapis_session_start_text_subtitle_load(session, first, 0) ==
          LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_start_text_subtitle_load(session, first, 1) == LAPIS_OK);
    std::uint32_t total = 0;
    CHECK(await_ready(session, first, total));
    CHECK(total == 2);

    lapis_text_subtitle_cue_info cue{};
    cue.start_nanoseconds = -33;
    std::uint32_t count{}, required{};
    lapis_text_subtitle_load_state state{};
    // Probe text required length; no writes before capacity validated.
    CHECK(lapis_session_read_text_subtitle_cues(
        session, first, 0, &cue, 1, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_E_BUFFER_TOO_SMALL);
    CHECK(state == LAPIS_TEXT_SUBTITLE_READY);
    CHECK(required == std::strlen("Hello embedded text"));
    CHECK(count == 0 && total == 2 && cue.start_nanoseconds == -33);

    std::array<char, 4096> bytes{};
    CHECK(lapis_session_read_text_subtitle_cues(
        session, first, 0, &cue, 1, bytes.data(),
        static_cast<std::uint32_t>(bytes.size()),
        &count, &total, &required, &state) == LAPIS_OK);
    CHECK(count == 1 && state == LAPIS_TEXT_SUBTITLE_READY);
    CHECK(cue.start_nanoseconds == 1'000'000'000);
    CHECK(cue.end_nanoseconds == 3'000'000'000);
    CHECK(cue.text_offset == 0 && cue.text_length == required);
    CHECK(std::string_view(bytes.data(), cue.text_length) == "Hello embedded text");

    CHECK(lapis_session_read_text_subtitle_cues(
        session, first, 1, &cue, 1, bytes.data(),
        static_cast<std::uint32_t>(bytes.size()),
        &count, &total, &required, &state) == LAPIS_OK);
    CHECK(count == 1 && total == 2);
    CHECK(cue.start_nanoseconds == 3'000'000'000);
    CHECK(std::string_view(bytes.data(), cue.text_length) == "Second cue");

    // Optional cancellation is request-ID scoped. An older completion
    // must not be able to stop a later selection at the *same* Epoch.
    CHECK(lapis_session_start_text_subtitle_load_with_id(
        session, first, 1, 0) == LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_start_text_subtitle_load_with_id(
        session, first, 1, 100) == LAPIS_OK);
    CHECK(await_ready(session, first, total));
    CHECK(lapis_session_cancel_text_subtitle_load(session, first, 99) == LAPIS_OK);
    CHECK(lapis_session_read_text_subtitle_cues(
        session, first, 0, nullptr, 0, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_OK);
    CHECK(state == LAPIS_TEXT_SUBTITLE_READY && total == 2);
    CHECK(lapis_session_cancel_text_subtitle_load(session, first, 100) == LAPIS_OK);
    CHECK(lapis_session_read_text_subtitle_cues(
        session, first, 0, nullptr, 0, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_OK);
    CHECK(state == LAPIS_TEXT_SUBTITLE_CANCELLED &&
          total == 0 && count == 0 && required == 0);
    CHECK(lapis_session_cancel_text_subtitle_load(session, first, 100) == LAPIS_OK);

    // Replace a request with a newer one for the same stream. The obsolete
    // request's delayed cancel may not erase the new decoder's snapshot.
    CHECK(lapis_session_start_text_subtitle_load_with_id(
        session, first, 1, 101) == LAPIS_OK);
    CHECK(lapis_session_start_text_subtitle_load_with_id(
        session, first, 1, 102) == LAPIS_OK);
    CHECK(lapis_session_cancel_text_subtitle_load(session, first, 101) == LAPIS_OK);
    CHECK(await_ready(session, first, total));
    CHECK(total == 2);
    CHECK(lapis_session_cancel_text_subtitle_load(session, first, 102) == LAPIS_OK);
    CHECK(lapis_session_read_text_subtitle_cues(
        session, first, 0, nullptr, 0, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_OK);
    CHECK(state == LAPIS_TEXT_SUBTITLE_CANCELLED);

    CHECK(lapis_session_cancel_text_subtitle_load(session, first, 0) ==
          LAPIS_E_INVALID_ARGUMENT);
    CHECK(lapis_session_cancel_text_subtitle_load(nullptr, first, 42) ==
          LAPIS_E_INVALID_ARGUMENT);

    // A seek invalidates the whole worker. Neither an old Epoch nor a new
    // Epoch without a fresh request may fetch a previously decoded cue.
    lapis_playback_epoch second{2};
    CHECK(lapis_session_seek(session, lapis_media_time{0}, second) == LAPIS_OK);
    CHECK(lapis_session_read_text_subtitle_cues(
        session, first, 0, nullptr, 0, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_E_STALE_EPOCH);
    CHECK(lapis_session_read_text_subtitle_cues(
        session, second, 0, nullptr, 0, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_E_INVALID_STATE);
    CHECK(lapis_session_start_text_subtitle_load(session, second, 1) == LAPIS_OK);
    CHECK(await_ready(session, second, total));
    CHECK(total == 2);

    // Invalid capacities and null pointers never write into caller buffers.
    CHECK(lapis_session_read_text_subtitle_cues(
        session, second, 0, nullptr, LAPIS_TEXT_SUBTITLE_MAX_PAGE + 1,
        nullptr, 0, &count, &total, &required, &state) ==
        LAPIS_E_INVALID_ARGUMENT);
    CHECK(count == 0);

    lapis_playback_epoch third{3};
    CHECK(lapis_session_close(session, third) == LAPIS_OK);
    CHECK(lapis_session_read_text_subtitle_cues(
        session, third, 0, nullptr, 0, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_E_INVALID_STATE);
    CHECK(lapis_session_cancel_text_subtitle_load(session, second, 102) ==
          LAPIS_E_STALE_EPOCH);
    CHECK(lapis_session_cancel_text_subtitle_load(session, third, 102) ==
          LAPIS_E_INVALID_STATE);
    CHECK(lapis_session_read_text_subtitle_cues(
        session, second, 0, nullptr, 0, nullptr, 0,
        &count, &total, &required, &state) == LAPIS_E_STALE_EPOCH);

    lapis_session_destroy(session);
    lapis_runtime_destroy(runtime);
    std::cout << "async C ABI text subtitle paging + Epoch/Close fence passed\n";
    return 0;
}
