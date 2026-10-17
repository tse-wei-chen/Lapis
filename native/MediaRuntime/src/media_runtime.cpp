#include "lapis/media_runtime.h"

#include "session/playback_media_session.hpp"
#include "source/media_source.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

struct lapis_runtime final {};

struct lapis_session final {
    std::mutex mutex;
    std::mutex media_operation_mutex;
    lapis_playback_session_id session_id{};
    uint64_t epoch{0};
    int64_t position_ns{0};
    lapis_transport_state transport{LAPIS_TRANSPORT_CLOSED};
    std::shared_ptr<lapis::media::PlaybackMediaSession> media;
};

namespace {

template <typename TAction>
lapis_result guarded_result(TAction&& action) noexcept {
    try {
        return std::forward<TAction>(action)();
    } catch (const std::bad_alloc&) {
        return LAPIS_E_OUT_OF_MEMORY;
    } catch (...) {
        return LAPIS_E_INTERNAL;
    }
}

bool advances_epoch(const lapis_session& session, lapis_playback_epoch epoch) noexcept {
    return epoch.value > session.epoch;
}

bool matches_epoch(const lapis_session& session, lapis_playback_epoch epoch) noexcept {
    return epoch.value == session.epoch;
}

void mark_faulted_if_current(
    lapis_session* session,
    lapis_playback_epoch epoch) noexcept {
    if (session == nullptr) {
        return;
    }

    try {
        std::scoped_lock lock(session->mutex);
        if (matches_epoch(*session, epoch)) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
        }
    } catch (...) {
        // This helper is used while translating an exception across the C ABI.
        // Never allow a secondary synchronization failure to escape.
    }
}

bool valid_session_id(const lapis_playback_session_id& session_id) noexcept {
    return std::any_of(
        std::begin(session_id.bytes),
        std::end(session_id.bytes),
        [](uint8_t value) { return value != 0; });
}

std::optional<lapis::media::MediaLocatorKind> to_core_locator_kind(
    lapis_media_locator_kind kind) noexcept {
    using lapis::media::MediaLocatorKind;

    switch (kind) {
        case LAPIS_LOCATOR_FILE:
            return MediaLocatorKind::File;
        case LAPIS_LOCATOR_DIRECTORY:
            return MediaLocatorKind::Directory;
        case LAPIS_LOCATOR_URI:
            return MediaLocatorKind::Uri;
        case LAPIS_LOCATOR_OPTICAL_DISC:
            return MediaLocatorKind::OpticalDisc;
    }

    return std::nullopt;
}

lapis_result map_open_error(lapis::media::MediaOpenError error) noexcept {
    using lapis::media::MediaOpenError;
    switch (error) {
        case MediaOpenError::None:
            return LAPIS_OK;
        case MediaOpenError::UnsupportedSource:
            return LAPIS_E_UNSUPPORTED;
        case MediaOpenError::SourceOpenFailed:
        case MediaOpenError::DemuxFailed:
        case MediaOpenError::DecoderFailed:
        case MediaOpenError::PacketRoutingFailed:
            return LAPIS_E_INTERNAL;
    }

    return LAPIS_E_INTERNAL;
}

} // namespace

uint32_t lapis_runtime_get_abi_version(void) {
    return LAPIS_MEDIA_RUNTIME_ABI_VERSION;
}

lapis_result lapis_runtime_create(lapis_runtime** out_runtime) {
    return guarded_result([&]() -> lapis_result {
        if (out_runtime == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        *out_runtime = new (std::nothrow) lapis_runtime{};
        return *out_runtime == nullptr ? LAPIS_E_OUT_OF_MEMORY : LAPIS_OK;
    });
}

void lapis_runtime_destroy(lapis_runtime* runtime) {
    delete runtime;
}

lapis_result lapis_session_create(
    lapis_runtime* runtime,
    lapis_playback_session_id session_id,
    lapis_session** out_session) {
    return guarded_result([&]() -> lapis_result {
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
    });
}

void lapis_session_destroy(lapis_session* session) {
    delete session;
}

lapis_result lapis_session_open(
    lapis_session* session,
    const lapis_media_locator* locator,
    lapis_playback_epoch epoch,
    lapis_open_result* out_result) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || locator == nullptr || out_result == nullptr ||
            locator->value_utf8 == nullptr || locator->value_utf8[0] == '\0') {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        const auto locator_kind = to_core_locator_kind(locator->kind);
        if (!locator_kind.has_value()) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::shared_ptr<lapis::media::PlaybackMediaSession> retired_media;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            session->epoch = epoch.value;
            session->position_ns = 0;
            session->transport = LAPIS_TRANSPORT_OPENING;
            retired_media = std::move(session->media);
            *out_result = {};
        }
        retired_media.reset();

        std::shared_ptr<lapis::media::PlaybackMediaSession> media;
        lapis::media::MediaOpenInfo media_open_result{};
        try {
            media = std::make_shared<lapis::media::PlaybackMediaSession>();
            const lapis::media::MediaLocator core_locator{
                *locator_kind,
                std::string(locator->value_utf8)
            };
            media_open_result = media->open(core_locator);
        } catch (...) {
            mark_faulted_if_current(session, epoch);
            throw;
        }

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }

        if (!media_open_result.ok()) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return map_open_error(media_open_result.error);
        }

        out_result->capabilities = 0;
        if (media_open_result.duration.has_value()) {
            out_result->duration.nanoseconds = media_open_result.duration->nanoseconds;
            out_result->has_duration = 1;
        }

        session->media = std::move(media);
        session->transport = LAPIS_TRANSPORT_READY;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_play(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
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
    });
}

lapis_result lapis_session_pause(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
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
    });
}

lapis_result lapis_session_seek(
    lapis_session* session,
    lapis_media_time position,
    lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || position.nanoseconds < 0) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        lapis_transport_state return_state{};
        std::shared_ptr<lapis::media::PlaybackMediaSession> media;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PLAYING &&
                session->transport != LAPIS_TRANSPORT_PAUSED) {
                return LAPIS_E_INVALID_STATE;
            }

            return_state = session->transport;
            session->epoch = epoch.value;
            session->transport = LAPIS_TRANSPORT_SEEKING;
            media = session->media;
        }

        bool seek_succeeded{};
        try {
            // State transitions remain protected by the short-lived session mutex,
            // while media mutations use a separate gate. Re-check the epoch after
            // acquiring that gate so a stale seek can never run after a newer seek
            // and leave the demuxer at the wrong physical position.
            std::scoped_lock media_lock(session->media_operation_mutex);
            {
                std::scoped_lock state_lock(session->mutex);
                if (!matches_epoch(*session, epoch)) {
                    return LAPIS_E_STALE_EPOCH;
                }
            }

            seek_succeeded = media != nullptr &&
                media->seek(lapis::media::MediaTime{position.nanoseconds});
        } catch (...) {
            mark_faulted_if_current(session, epoch);
            throw;
        }

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }

        if (!seek_succeeded) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return LAPIS_E_INTERNAL;
        }

        session->position_ns = position.nanoseconds;
        session->transport = return_state;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_close(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::shared_ptr<lapis::media::PlaybackMediaSession> retired_media;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            session->epoch = epoch.value;
            session->position_ns = 0;
            session->transport = LAPIS_TRANSPORT_CLOSED;
            retired_media = std::move(session->media);
        }
        retired_media.reset();
        return LAPIS_OK;
    });
}

lapis_result lapis_session_get_snapshot(
    lapis_session* session,
    lapis_playback_snapshot* out_snapshot) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || out_snapshot == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock lock(session->mutex);
        out_snapshot->session_id = session->session_id;
        out_snapshot->epoch.value = session->epoch;
        out_snapshot->position.nanoseconds = session->position_ns;
        out_snapshot->transport = session->transport;
        return LAPIS_OK;
    });
}
