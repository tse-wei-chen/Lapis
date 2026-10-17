#include "lapis/media_runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <new>

struct lapis_runtime final {};

struct lapis_session final {
    std::mutex mutex;
    lapis_playback_session_id session_id{};
    uint64_t epoch{0};
    int64_t position_ns{0};
    lapis_transport_state transport{LAPIS_TRANSPORT_CLOSED};
};

namespace {

bool advances_epoch(const lapis_session& session, lapis_playback_epoch epoch) noexcept {
    return epoch.value > session.epoch;
}

bool matches_epoch(const lapis_session& session, lapis_playback_epoch epoch) noexcept {
    return epoch.value == session.epoch;
}

bool valid_session_id(const lapis_playback_session_id& session_id) noexcept {
    return std::any_of(
        std::begin(session_id.bytes),
        std::end(session_id.bytes),
        [](uint8_t value) { return value != 0; });
}

bool valid_locator_kind(lapis_media_locator_kind kind) noexcept {
    return kind >= LAPIS_LOCATOR_FILE && kind <= LAPIS_LOCATOR_OPTICAL_DISC;
}

} // namespace

uint32_t lapis_runtime_get_abi_version(void) {
    return LAPIS_MEDIA_RUNTIME_ABI_VERSION;
}

lapis_result lapis_runtime_create(lapis_runtime** out_runtime) {
    if (out_runtime == nullptr) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    *out_runtime = new (std::nothrow) lapis_runtime{};
    return *out_runtime == nullptr ? LAPIS_E_OUT_OF_MEMORY : LAPIS_OK;
}

void lapis_runtime_destroy(lapis_runtime* runtime) {
    delete runtime;
}

lapis_result lapis_session_create(
    lapis_runtime* runtime,
    lapis_playback_session_id session_id,
    lapis_session** out_session) {
    if (runtime == nullptr || out_session == nullptr || !valid_session_id(session_id)) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    auto* session = new (std::nothrow) lapis_session{};
    if (session == nullptr) {
        return LAPIS_E_OUT_OF_MEMORY;
    }

    session->session_id = session_id;
    *out_session = session;
    return LAPIS_OK;
}

void lapis_session_destroy(lapis_session* session) {
    delete session;
}

lapis_result lapis_session_open(
    lapis_session* session,
    const lapis_media_locator* locator,
    lapis_playback_epoch epoch,
    lapis_open_result* out_result) {
    if (session == nullptr || locator == nullptr || out_result == nullptr ||
        locator->value_utf8 == nullptr || locator->value_utf8[0] == '\0' ||
        !valid_locator_kind(locator->kind)) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    std::scoped_lock lock(session->mutex);
    if (!advances_epoch(*session, epoch)) {
        return LAPIS_E_STALE_EPOCH;
    }

    session->epoch = epoch.value;
    session->position_ns = 0;
    session->transport = LAPIS_TRANSPORT_OPENING;

    // Source selection and demux adapters are connected in the next milestone.
    // Keep the source kind explicit at this boundary so adapters never need to
    // infer file/disc/network semantics from a path-shaped string.
    out_result->capabilities = 0;
    out_result->duration = {0};
    out_result->has_duration = 0;

    session->transport = LAPIS_TRANSPORT_READY;
    return LAPIS_OK;
}

lapis_result lapis_session_play(lapis_session* session, lapis_playback_epoch epoch) {
    if (session == nullptr) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    std::scoped_lock lock(session->mutex);
    if (!matches_epoch(*session, epoch)) {
        return LAPIS_E_STALE_EPOCH;
    }

    if (session->transport != LAPIS_TRANSPORT_READY &&
        session->transport != LAPIS_TRANSPORT_PAUSED) {
        return LAPIS_E_INVALID_STATE;
    }

    session->transport = LAPIS_TRANSPORT_PLAYING;
    return LAPIS_OK;
}

lapis_result lapis_session_pause(lapis_session* session, lapis_playback_epoch epoch) {
    if (session == nullptr) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    std::scoped_lock lock(session->mutex);
    if (!matches_epoch(*session, epoch)) {
        return LAPIS_E_STALE_EPOCH;
    }

    if (session->transport != LAPIS_TRANSPORT_PLAYING) {
        return LAPIS_E_INVALID_STATE;
    }

    session->transport = LAPIS_TRANSPORT_PAUSED;
    return LAPIS_OK;
}

lapis_result lapis_session_seek(
    lapis_session* session,
    lapis_media_time position,
    lapis_playback_epoch epoch) {
    if (session == nullptr || position.nanoseconds < 0) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    std::scoped_lock lock(session->mutex);
    if (!advances_epoch(*session, epoch)) {
        return LAPIS_E_STALE_EPOCH;
    }

    if (session->transport != LAPIS_TRANSPORT_READY &&
        session->transport != LAPIS_TRANSPORT_PLAYING &&
        session->transport != LAPIS_TRANSPORT_PAUSED) {
        return LAPIS_E_INVALID_STATE;
    }

    const auto return_state = session->transport;
    session->epoch = epoch.value;
    session->transport = LAPIS_TRANSPORT_SEEKING;
    session->position_ns = position.nanoseconds;
    session->transport = return_state;
    return LAPIS_OK;
}

lapis_result lapis_session_close(lapis_session* session, lapis_playback_epoch epoch) {
    if (session == nullptr) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    std::scoped_lock lock(session->mutex);
    if (!advances_epoch(*session, epoch)) {
        return LAPIS_E_STALE_EPOCH;
    }

    session->epoch = epoch.value;
    session->position_ns = 0;
    session->transport = LAPIS_TRANSPORT_CLOSED;
    return LAPIS_OK;
}

lapis_result lapis_session_get_snapshot(
    lapis_session* session,
    lapis_playback_snapshot* out_snapshot) {
    if (session == nullptr || out_snapshot == nullptr) {
        return LAPIS_E_INVALID_ARGUMENT;
    }

    std::scoped_lock lock(session->mutex);
    out_snapshot->session_id = session->session_id;
    out_snapshot->epoch.value = session->epoch;
    out_snapshot->position.nanoseconds = session->position_ns;
    out_snapshot->transport = session->transport;
    return LAPIS_OK;
}
