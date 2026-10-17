#include "lapis/media_runtime.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <thread>

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

    lapis_runtime* runtime = nullptr;
    CHECK(lapis_runtime_create(&runtime) == LAPIS_OK);
    CHECK(runtime != nullptr);

    const auto session_id = test_session_id();
    lapis_session* session = nullptr;
    CHECK(lapis_session_create(runtime, session_id, &session) == LAPIS_OK);
    CHECK(session != nullptr);

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

    // Seeking while READY prepares a stopped PlaybackExecution timeline. Play
    // must use that same epoch; beginning presentation is not a discontinuity.
    CHECK(lapis_session_seek(session, {0}, {2}) == LAPIS_OK);
    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_READY);
    CHECK(snapshot.epoch.value == 2);
    CHECK(snapshot.position.nanoseconds == 0);

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
    CHECK(snapshot.position.nanoseconds == 0);

    // Natural Ended/Faulted publication belongs to the serialized command path.
    // Do not call get_snapshot while the tiny fixture drains: pause() itself must
    // refresh the execution terminal state before validating PLAYING, rather than
    // depending on a previous snapshot read to mutate session transport.
    open_result = {};
    CHECK(lapis_session_open(session, &locator, {4}, &open_result) == LAPIS_OK);
    CHECK(lapis_session_play(session, {4}) == LAPIS_OK);
    std::this_thread::sleep_for(250ms);
    CHECK(lapis_session_pause(session, {4}) == LAPIS_E_INVALID_STATE);

    CHECK(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    CHECK(snapshot.transport == LAPIS_TRANSPORT_ENDED);
    CHECK(snapshot.epoch.value == 4);

    CHECK(lapis_session_close(session, {5}) == LAPIS_OK);

    lapis_session_destroy(session);
    lapis_runtime_destroy(runtime);
    return 0;
}
