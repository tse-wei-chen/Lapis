#include "lapis/media_runtime.h"

#include <cassert>
#include <cstddef>

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
    assert(lapis_runtime_get_abi_version() == LAPIS_MEDIA_RUNTIME_ABI_VERSION);

    lapis_runtime* runtime = nullptr;
    assert(lapis_runtime_create(&runtime) == LAPIS_OK);
    assert(runtime != nullptr);

    const auto session_id = test_session_id();
    lapis_session* session = nullptr;
    assert(lapis_session_create(runtime, session_id, &session) == LAPIS_OK);
    assert(session != nullptr);

    lapis_playback_snapshot snapshot{};
    assert(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    assert(same_session_id(snapshot.session_id, session_id));
    assert(snapshot.transport == LAPIS_TRANSPORT_CLOSED);
    assert(snapshot.epoch.value == 0);

    const lapis_media_locator locator{LAPIS_LOCATOR_FILE, "sample.mkv"};
    lapis_open_result open_result{};
    assert(lapis_session_open(session, &locator, {1}, &open_result) == LAPIS_OK);
    assert(open_result.capabilities == 0);
    assert(open_result.has_duration == 0);

    assert(lapis_session_play(session, {1}) == LAPIS_OK);
    assert(lapis_session_seek(session, {5'000'000'000LL}, {2}) == LAPIS_OK);

    // Commands captured before a discontinuity must not mutate the new epoch.
    assert(lapis_session_pause(session, {1}) == LAPIS_E_STALE_EPOCH);
    assert(lapis_session_play(session, {1}) == LAPIS_E_STALE_EPOCH);

    assert(lapis_session_get_snapshot(session, &snapshot) == LAPIS_OK);
    assert(snapshot.transport == LAPIS_TRANSPORT_PLAYING);
    assert(snapshot.epoch.value == 2);
    assert(snapshot.position.nanoseconds == 5'000'000'000LL);

    assert(lapis_session_seek(session, {6'000'000'000LL}, {1}) == LAPIS_E_STALE_EPOCH);
    assert(lapis_session_seek(session, {-1}, {3}) == LAPIS_E_INVALID_ARGUMENT);
    assert(lapis_session_close(session, {3}) == LAPIS_OK);

    lapis_session_destroy(session);
    lapis_runtime_destroy(runtime);
    return 0;
}
