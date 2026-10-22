#include "lapis/media_runtime.h"

#include "core/diagnostic_events.hpp"
#include "navigation/chapter_seek.hpp"
#include "execution/playback_execution.hpp"
#include "session/playback_media_session.hpp"
#include "session/async_text_subtitle_timeline.hpp"
#include "source/media_source.hpp"
#include "video/video_output_surface.hpp"
#include "platform/playback_execution_factory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

struct lapis_runtime final {};

namespace {

struct RuntimePlaybackPipeline final {
    std::shared_ptr<lapis::media::PlaybackMediaSession> media;
    std::unique_ptr<lapis::media::PlaybackExecution> execution;
    // Immutable media identity for the independent local-file subtitle reader.
    lapis::media::MediaLocator subtitle_source;
    // Destroyed before execution/media: jthread stops and joins first.
    std::unique_ptr<lapis::media::AsyncTextSubtitleTimeline> subtitle_timeline;
    uint64_t subtitle_request_id{0}; // Zero marks the legacy v1 loader.
};

} // namespace

struct lapis_session final {
    std::mutex mutex;
    // Result-returning control commands are serialized at the C ABI boundary.
    // PlaybackExecution remains independently synchronized, while snapshots only
    // take the short-lived state mutex below.
    std::mutex command_mutex;
    lapis_playback_session_id session_id{};
    uint64_t epoch{0};
    int64_t position_ns{0};
    lapis_transport_state transport{LAPIS_TRANSPORT_CLOSED};
    std::shared_ptr<RuntimePlaybackPipeline> pipeline;
    lapis::media::VideoOutputSurfaceDescriptor desired_output_surface{};
    std::shared_ptr<lapis::media::DiagnosticEventJournal> diagnostics{
        std::make_shared<lapis::media::DiagnosticEventJournal>()
    };
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

std::optional<lapis::media::VideoOutputSurfaceDescriptor> to_core_output_surface(
    lapis_video_output_surface surface) noexcept {
    using lapis::media::VideoOutputSurfaceDescriptor;
    using lapis::media::VideoOutputSurfaceKind;
    VideoOutputSurfaceDescriptor descriptor{};
    switch (surface.kind) {
        case LAPIS_VIDEO_OUTPUT_SURFACE_NONE:
            descriptor.kind = VideoOutputSurfaceKind::None;
            break;
        case LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW:
            descriptor.kind = VideoOutputSurfaceKind::Win32Window;
            break;
        default:
            return std::nullopt;
    }
    descriptor.native_handle = surface.native_handle;
    if (!lapis::media::valid_video_output_surface(descriptor)) {
        return std::nullopt;
    }
    return descriptor;
}

lapis_result map_surface_control_error(
    lapis::media::VideoOutputSurfaceControlStatus status) noexcept {
    using lapis::media::VideoOutputSurfaceControlStatus;
    switch (status) {
        case VideoOutputSurfaceControlStatus::Applied:
        case VideoOutputSurfaceControlStatus::Deferred:
            return LAPIS_OK;
        case VideoOutputSurfaceControlStatus::Invalid:
            return LAPIS_E_INVALID_ARGUMENT;
        case VideoOutputSurfaceControlStatus::Unsupported:
            return LAPIS_E_UNSUPPORTED;
        case VideoOutputSurfaceControlStatus::Unavailable:
            return LAPIS_E_OUTPUT_UNAVAILABLE;
        case VideoOutputSurfaceControlStatus::Failed:
            return LAPIS_E_INTERNAL;
    }
    return LAPIS_E_INTERNAL;
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

lapis_result map_execution_error(
    lapis::media::PlaybackExecutionError error) noexcept {
    using lapis::media::PlaybackExecutionError;
    switch (error) {
        case PlaybackExecutionError::None:
            return LAPIS_OK;
        case PlaybackExecutionError::InvalidArgument:
            return LAPIS_E_INVALID_ARGUMENT;
        case PlaybackExecutionError::InvalidState:
            return LAPIS_E_INVALID_STATE;
        case PlaybackExecutionError::StaleEpoch:
            return LAPIS_E_STALE_EPOCH;
        case PlaybackExecutionError::MediaSeekFailed:
        case PlaybackExecutionError::ClockFailure:
        case PlaybackExecutionError::WorkerStartFailed:
        case PlaybackExecutionError::WorkerFailed:
            return LAPIS_E_INTERNAL;
    }

    return LAPIS_E_INTERNAL;
}

std::shared_ptr<RuntimePlaybackPipeline> create_pipeline(
    std::shared_ptr<lapis::media::PlaybackMediaSession> media,
    std::shared_ptr<lapis::media::DiagnosticEventJournal> journal,
    lapis::media::MediaLocator subtitle_source) {
    auto pipeline = std::make_shared<RuntimePlaybackPipeline>();
    pipeline->subtitle_source = std::move(subtitle_source);
    pipeline->media = std::move(media);
    pipeline->execution = lapis::media::platform::create_execution(
        *pipeline->media, std::move(journal));
    return pipeline;
}

lapis_transport_state transport_from_execution_state(
    lapis::media::PlaybackExecutionState state,
    lapis_transport_state fallback) noexcept {
    using lapis::media::PlaybackExecutionState;
    switch (state) {
        case PlaybackExecutionState::Running:
            return LAPIS_TRANSPORT_PLAYING;
        case PlaybackExecutionState::Paused:
            return LAPIS_TRANSPORT_PAUSED;
        case PlaybackExecutionState::Ended:
            return LAPIS_TRANSPORT_ENDED;
        case PlaybackExecutionState::Faulted:
            return LAPIS_TRANSPORT_FAULTED;
        case PlaybackExecutionState::Stopped:
            return fallback;
    }

    return fallback;
}

bool terminal_transport(lapis_transport_state transport) noexcept {
    return transport == LAPIS_TRANSPORT_ENDED ||
        transport == LAPIS_TRANSPORT_FAULTED;
}

bool active_transport(lapis_transport_state transport) noexcept {
    return transport == LAPIS_TRANSPORT_PLAYING ||
        transport == LAPIS_TRANSPORT_PAUSED;
}

// Control commands call this while holding command_mutex. Natural completion is
// asynchronous, so publish it through the same single-writer command path before
// validating the next mutation. Snapshot reads stay observational and never write
// session transport behind a command's back.
void refresh_terminal_transport(lapis_session* session) {
    std::shared_ptr<RuntimePlaybackPipeline> pipeline;
    lapis_transport_state observed_transport{};
    uint64_t observed_epoch{};

    {
        std::scoped_lock lock(session->mutex);
        if (!active_transport(session->transport) ||
            session->pipeline == nullptr ||
            session->pipeline->execution == nullptr) {
            return;
        }

        pipeline = session->pipeline;
        observed_transport = session->transport;
        observed_epoch = session->epoch;
    }

    const auto execution_snapshot = pipeline->execution->snapshot();
    const auto effective_transport = transport_from_execution_state(
        execution_snapshot.state,
        observed_transport);
    if (!terminal_transport(effective_transport)) {
        return;
    }

    std::scoped_lock lock(session->mutex);
    if (session->epoch != observed_epoch ||
        session->pipeline != pipeline ||
        !active_transport(session->transport)) {
        return;
    }

    session->position_ns = execution_snapshot.clock.position.nanoseconds;
    session->transport = effective_transport;
}

} // namespace

uint32_t lapis_runtime_get_abi_version(void) {
    return LAPIS_MEDIA_RUNTIME_ABI_VERSION;
}

uint32_t lapis_runtime_get_source_features(void) {
    uint32_t features = LAPIS_SOURCE_FEATURE_FILE;
#if defined(LAPIS_ENABLE_LIBBLURAY)
    features |= LAPIS_SOURCE_FEATURE_BDMV_DIRECTORY;
#endif
    return features;
}

uint32_t lapis_runtime_get_diagnostics_api_version(void) {
    return LAPIS_DIAGNOSTICS_API_VERSION;
}

uint32_t lapis_runtime_get_navigation_api_version(void) {
    return LAPIS_NAVIGATION_API_VERSION;
}

uint32_t lapis_runtime_get_subtitle_tracks_api_version(void) {
    return LAPIS_SUBTITLE_TRACKS_API_VERSION;
}

uint32_t lapis_runtime_get_text_subtitle_cues_api_version(void) {
    return LAPIS_TEXT_SUBTITLE_CUES_API_VERSION;
}

uint32_t lapis_runtime_get_text_subtitle_cancel_api_version(void) {
    return LAPIS_TEXT_SUBTITLE_CANCEL_API_VERSION;
}

uint32_t lapis_runtime_get_fast_forward_api_version(void) {
    return LAPIS_FAST_FORWARD_API_VERSION;
}

lapis_result lapis_session_list_subtitle_tracks(
    lapis_session* session,
    lapis_playback_epoch expected_epoch,
    lapis_subtitle_track_info* tracks,
    uint32_t capacity,
    uint32_t* out_count) {
    return guarded_result([&]() -> lapis_result {
        if (out_count != nullptr) *out_count = 0;
        if (session == nullptr || out_count == nullptr ||
            capacity > LAPIS_SUBTITLE_TRACK_MAX ||
            (capacity != 0 && tracks == nullptr)) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        // The immutable snapshot belongs to the opened playback pipeline.
        // Serializing with commands also fences a concurrent Seek/Close/Open.
        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, expected_epoch))
                return LAPIS_E_STALE_EPOCH;
            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PLAYING &&
                session->transport != LAPIS_TRANSPORT_PAUSED &&
                session->transport != LAPIS_TRANSPORT_ENDED) {
                return LAPIS_E_INVALID_STATE;
            }
            pipeline = session->pipeline;
        }
        if (pipeline == nullptr || pipeline->media == nullptr)
            return LAPIS_E_INVALID_STATE;

        const auto found = pipeline->media->subtitle_streams();
        *out_count = static_cast<uint32_t>(found.size());
        if (capacity == 0) return LAPIS_OK; // count-only probe, including 0 tracks
        if (capacity < found.size()) return LAPIS_E_BUFFER_TOO_SMALL;

        // ISO 639/BCP47 language tags and FFmpeg codec names are ASCII.
        // Non-ASCII bytes are replaced with '?', ensuring valid UTF-8 output
        // without truncating a multibyte sequence at the fixed-size ABI edge.
        const auto copy_ascii = [](char* destination, std::size_t bound,
                                   std::string_view value) {
            std::size_t i = 0;
            for (unsigned char ch : value) {
                if (i + 1 >= bound) break;
                destination[i++] =
                    ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : '?';
            }
            destination[i] = '\0';
        };
        for (std::size_t i = 0; i < found.size(); ++i) {
            const auto& source = found[i];
            lapis_subtitle_track_info dest{};
            dest.stream_index = source.stream_index;
            dest.kind = static_cast<uint32_t>(source.kind);
            copy_ascii(dest.language_utf8, sizeof(dest.language_utf8), source.language);
            copy_ascii(dest.codec_name_utf8, sizeof(dest.codec_name_utf8),
                       source.codec_name);
            tracks[i] = dest;
        }
        return LAPIS_OK;
    });
}


static lapis_result start_text_subtitle_load_impl(
    lapis_session* session, lapis_playback_epoch expected_epoch,
    uint32_t stream_index, uint64_t request_id) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr ||
            stream_index > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            return LAPIS_E_INVALID_ARGUMENT;

        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, expected_epoch))
                return LAPIS_E_STALE_EPOCH;
            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PLAYING &&
                session->transport != LAPIS_TRANSPORT_PAUSED &&
                session->transport != LAPIS_TRANSPORT_ENDED)
                return LAPIS_E_INVALID_STATE;
            pipeline = session->pipeline;
        }
        if (pipeline == nullptr || pipeline->media == nullptr)
            return LAPIS_E_INVALID_STATE;
        if (pipeline->subtitle_source.kind != lapis::media::MediaLocatorKind::File)
            return LAPIS_E_UNSUPPORTED;

        const auto subtitles = pipeline->media->subtitle_streams();
        const auto track = std::find_if(subtitles.begin(), subtitles.end(),
            [stream_index](const lapis::media::SubtitleStreamInfo& item) {
                return item.stream_index == stream_index;
            });
        if (track == subtitles.end()) return LAPIS_E_INVALID_ARGUMENT;
        if (track->kind != lapis::media::SubtitleTrackKind::Text ||
            (track->codec_name != "subrip" && track->codec_name != "mov_text"))
            return LAPIS_E_UNSUPPORTED;

        // No packet queue is added to the live A/V decoder. A new request
        // retires the previous worker first, even within the same Epoch.
        pipeline->subtitle_timeline.reset();
        pipeline->subtitle_timeline =
            std::make_unique<lapis::media::AsyncTextSubtitleTimeline>(
                pipeline->subtitle_source, static_cast<int>(stream_index));
        // Published only after construction succeeds. Session commands are
        // serialized; a late cancel for an old request is a no-op.
        pipeline->subtitle_request_id = request_id;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_start_text_subtitle_load(
    lapis_session* session, lapis_playback_epoch expected_epoch,
    uint32_t stream_index) {
    return start_text_subtitle_load_impl(session, expected_epoch, stream_index, 0);
}

lapis_result lapis_session_start_text_subtitle_load_with_id(
    lapis_session* session, lapis_playback_epoch expected_epoch,
    uint32_t stream_index, uint64_t request_id) {
    if (request_id == 0) return LAPIS_E_INVALID_ARGUMENT;
    return start_text_subtitle_load_impl(
        session, expected_epoch, stream_index, request_id);
}

lapis_result lapis_session_cancel_text_subtitle_load(
    lapis_session* session, lapis_playback_epoch expected_epoch,
    uint64_t request_id) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || request_id == 0) return LAPIS_E_INVALID_ARGUMENT;

        // Serialize against Open, Seek, Close and a new subtitle selection.
        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, expected_epoch))
                return LAPIS_E_STALE_EPOCH;
            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PLAYING &&
                session->transport != LAPIS_TRANSPORT_PAUSED &&
                session->transport != LAPIS_TRANSPORT_ENDED)
                return LAPIS_E_INVALID_STATE;
            pipeline = session->pipeline;
        }
        if (pipeline == nullptr) return LAPIS_E_INVALID_STATE;

        // A stale cancellation must never stop a newer choice (including
        // reselecting the same track ID). No separate lock or freed pointer.
        if (pipeline->subtitle_timeline != nullptr &&
            pipeline->subtitle_request_id == request_id)
            pipeline->subtitle_timeline->cancel(); // request_stop, no join
        return LAPIS_OK;
    });
}

lapis_result lapis_session_read_text_subtitle_cues(
    lapis_session* session,
    lapis_playback_epoch expected_epoch,
    uint32_t start_index,
    lapis_text_subtitle_cue_info* cues,
    uint32_t cue_capacity,
    char* text_utf8,
    uint32_t text_capacity,
    uint32_t* out_count,
    uint32_t* out_total,
    uint32_t* out_required_text_bytes,
    lapis_text_subtitle_load_state* out_state) {
    return guarded_result([&]() -> lapis_result {
        if (out_count) *out_count = 0;
        if (out_total) *out_total = 0;
        if (out_required_text_bytes) *out_required_text_bytes = 0;
        if (out_state) *out_state = LAPIS_TEXT_SUBTITLE_LOADING;
        if (session == nullptr || out_count == nullptr ||
            out_total == nullptr || out_required_text_bytes == nullptr ||
            out_state == nullptr ||
            cue_capacity > LAPIS_TEXT_SUBTITLE_MAX_PAGE ||
            text_capacity > LAPIS_TEXT_SUBTITLE_MAX_PAGE_BYTES ||
            (cue_capacity != 0 && cues == nullptr) ||
            (text_capacity != 0 && text_utf8 == nullptr))
            return LAPIS_E_INVALID_ARGUMENT;

        // Use the same command lock as Open/Seek/Close. A returned page can
        // never belong to an epoch that changed during this call.
        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, expected_epoch))
                return LAPIS_E_STALE_EPOCH;
            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PLAYING &&
                session->transport != LAPIS_TRANSPORT_PAUSED &&
                session->transport != LAPIS_TRANSPORT_ENDED)
                return LAPIS_E_INVALID_STATE;
            pipeline = session->pipeline;
        }
        if (pipeline == nullptr || pipeline->subtitle_timeline == nullptr)
            return LAPIS_E_INVALID_STATE;

        const auto page = pipeline->subtitle_timeline->read_page(
            start_index, cue_capacity);
        using lapis::media::TextSubtitleTimelineState;
        switch (page.state) {
            case TextSubtitleTimelineState::Loading:
                *out_state = LAPIS_TEXT_SUBTITLE_LOADING; break;
            case TextSubtitleTimelineState::Ready:
                *out_state = LAPIS_TEXT_SUBTITLE_READY; break;
            case TextSubtitleTimelineState::Failed:
                *out_state = LAPIS_TEXT_SUBTITLE_FAILED; break;
            case TextSubtitleTimelineState::LimitExceeded:
                *out_state = LAPIS_TEXT_SUBTITLE_LIMIT_EXCEEDED; break;
            case TextSubtitleTimelineState::Cancelled:
                *out_state = LAPIS_TEXT_SUBTITLE_CANCELLED; break;
        }
        *out_total = static_cast<uint32_t>(page.total);
        if (page.state != TextSubtitleTimelineState::Ready ||
            page.cues.empty())
            return LAPIS_OK;

        std::size_t required = 0;
        for (const auto& cue : page.cues) required += cue.text.size();
        if (required > LAPIS_TEXT_SUBTITLE_MAX_PAGE_BYTES)
            return LAPIS_E_INTERNAL;
        *out_required_text_bytes = static_cast<uint32_t>(required);
        if (text_capacity < required)
            return LAPIS_E_BUFFER_TOO_SMALL;

        uint32_t offset = 0;
        for (std::size_t i = 0; i < page.cues.size(); ++i) {
            const auto& cue = page.cues[i];
            lapis_text_subtitle_cue_info item{};
            item.start_nanoseconds = cue.start.nanoseconds;
            item.end_nanoseconds = cue.end.nanoseconds;
            item.text_offset = offset;
            item.text_length = static_cast<uint32_t>(cue.text.size());
            if (!cue.text.empty())
                std::memcpy(text_utf8 + offset, cue.text.data(), cue.text.size());
            cues[i] = item;
            offset += item.text_length;
        }
        *out_count = static_cast<uint32_t>(page.cues.size());
        return LAPIS_OK;
    });
}

lapis_result lapis_session_resolve_chapter(
    lapis_session* session,
    uint32_t chapter_id,
    lapis_playback_epoch expected_epoch,
    lapis_media_time* out_position) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || out_position == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }
        *out_position = {};

        // Serialize with Open/Seek/Close so the source cannot be retired
        // while its navigation metadata is being copied.
        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, expected_epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }
            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PLAYING &&
                session->transport != LAPIS_TRANSPORT_PAUSED) {
                return LAPIS_E_INVALID_STATE;
            }
            pipeline = session->pipeline;
        }
        if (pipeline == nullptr || pipeline->media == nullptr) {
            return LAPIS_E_UNSUPPORTED;
        }

        const auto state = pipeline->media->navigation_snapshot();
        if (!state.has_value() ||
            !lapis::media::supports_navigation(
                state->capabilities, lapis::media::NavigationCapability::Chapters)) {
            return LAPIS_E_UNSUPPORTED;
        }
        const auto start = lapis::media::selected_chapter_start(*state, chapter_id);
        if (!start.has_value()) {
            return LAPIS_E_INVALID_ARGUMENT;
        }
        out_position->nanoseconds = start->nanoseconds;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_read_diagnostic_events(
    lapis_session* session,
    uint64_t since_sequence,
    lapis_diagnostic_event* events,
    uint32_t capacity,
    uint32_t* out_count,
    uint64_t* out_next_sequence,
    uint64_t* out_dropped) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || out_count == nullptr ||
            out_next_sequence == nullptr || out_dropped == nullptr ||
            capacity > LAPIS_DIAGNOSTIC_RING_CAPACITY ||
            (capacity != 0 && events == nullptr)) {
            return LAPIS_E_INVALID_ARGUMENT;
        }
        // No dependency on command_mutex, media decode or D3D locks.
        // This session-scoped journal persists over close/open generations.
        lapis::media::DiagnosticEvent buffer[
            lapis::media::DiagnosticEventJournal::capacity]{};
        uint64_t next{};
        uint64_t dropped{};
        const auto count = session->diagnostics->read_after(
            since_sequence, buffer, capacity, next, dropped);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& source = buffer[i];
            events[i] = {
                source.sequence, source.epoch, source.monotonic_milliseconds,
                static_cast<uint32_t>(source.kind), source.failure_stage,
                source.hresult, source.removed_reason, source.detail
            };
        }
        *out_count = count;
        *out_next_sequence = next;
        *out_dropped = dropped;
        return LAPIS_OK;
    });
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

        std::scoped_lock command_lock(session->command_mutex);

        std::shared_ptr<RuntimePlaybackPipeline> retired_pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            session->epoch = epoch.value;
            session->position_ns = 0;
            session->transport = LAPIS_TRANSPORT_OPENING;
            retired_pipeline = std::move(session->pipeline);
            *out_result = {};
        }
        // Stop/join an old execution outside the state mutex. Snapshot readers
        // can still observe OPENING instead of being blocked behind teardown.
        retired_pipeline.reset();

        std::shared_ptr<lapis::media::PlaybackMediaSession> media;
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        lapis::media::MediaOpenInfo media_open_result{};
        try {
            media = std::make_shared<lapis::media::PlaybackMediaSession>();
            const lapis::media::MediaLocator core_locator{
                *locator_kind,
                std::string(locator->value_utf8)
            };
            media_open_result = media->open(core_locator);
            if (media_open_result.ok()) {
                pipeline = create_pipeline(media, session->diagnostics, core_locator);
                // The desired host window belongs to the logical session,
                // not to a particular media-open or decode generation.
                const auto surface_status = pipeline->execution->set_video_output_surface(
                    session->desired_output_surface);
                if (surface_status != lapis::media::VideoOutputSurfaceControlStatus::Applied &&
                    surface_status != lapis::media::VideoOutputSurfaceControlStatus::Deferred) {
                    mark_faulted_if_current(session, epoch);
                    return map_surface_control_error(surface_status);
                }
            }
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
        if (pipeline == nullptr || pipeline->execution == nullptr) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return LAPIS_E_INTERNAL;
        }

        out_result->capabilities = 0;
        if (media_open_result.duration.has_value()) {
            out_result->duration.nanoseconds = media_open_result.duration->nanoseconds;
            out_result->has_duration = 1;
        }

        session->pipeline = std::move(pipeline);
        session->transport = LAPIS_TRANSPORT_READY;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_play(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);
        refresh_terminal_transport(session);

        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        lapis_transport_state prior_transport{};
        int64_t position_ns{};
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            if (session->transport != LAPIS_TRANSPORT_READY &&
                session->transport != LAPIS_TRANSPORT_PAUSED) {
                return LAPIS_E_INVALID_STATE;
            }
            if (session->pipeline == nullptr ||
                session->pipeline->execution == nullptr) {
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return LAPIS_E_INTERNAL;
            }

            prior_transport = session->transport;
            position_ns = session->position_ns;
            pipeline = session->pipeline;
        }

        const auto result = prior_transport == LAPIS_TRANSPORT_READY
            ? pipeline->execution->start(
                lapis::media::PlaybackEpoch{epoch.value},
                lapis::media::MediaTime{position_ns})
            : pipeline->execution->resume();
        if (!result.ok()) {
            const auto mapped = map_execution_error(result.error);
            mark_faulted_if_current(session, epoch);
            return mapped;
        }

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
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

        std::scoped_lock command_lock(session->command_mutex);
        refresh_terminal_transport(session);

        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }

            if (session->transport != LAPIS_TRANSPORT_PLAYING) {
                return LAPIS_E_INVALID_STATE;
            }
            if (session->pipeline == nullptr ||
                session->pipeline->execution == nullptr) {
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return LAPIS_E_INTERNAL;
            }
            pipeline = session->pipeline;
        }

        const auto result = pipeline->execution->pause();
        if (!result.ok()) {
            const auto mapped = map_execution_error(result.error);
            mark_faulted_if_current(session, epoch);
            return mapped;
        }
        const auto execution_snapshot = pipeline->execution->snapshot();

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }
        session->position_ns = execution_snapshot.clock.position.nanoseconds;
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

        std::scoped_lock command_lock(session->command_mutex);
        refresh_terminal_transport(session);

        lapis_transport_state return_state{};
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
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
            if (session->pipeline == nullptr ||
                session->pipeline->execution == nullptr) {
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return LAPIS_E_INTERNAL;
            }

            return_state = session->transport;
            session->epoch = epoch.value;
            session->transport = LAPIS_TRANSPORT_SEEKING;
            pipeline = session->pipeline;
        }

        // A Seek increments the public Epoch. Retire the independent cue
        // worker before the media graph moves to the new generation.
        pipeline->subtitle_timeline.reset();

        lapis::media::PlaybackExecutionResult seek_result{};
        try {
            seek_result = pipeline->execution->seek(
                lapis::media::PlaybackEpoch{epoch.value},
                lapis::media::MediaTime{position.nanoseconds});
        } catch (...) {
            mark_faulted_if_current(session, epoch);
            throw;
        }

        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }

        if (!seek_result.ok()) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return map_execution_error(seek_result.error);
        }

        session->position_ns = position.nanoseconds;
        session->transport = return_state;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_set_fast_forward(
    lapis_session* session,
    lapis_playback_epoch expected_epoch,
    uint32_t enabled) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || enabled > 1) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, expected_epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }
            if (session->transport != LAPIS_TRANSPORT_PLAYING ||
                session->pipeline == nullptr ||
                session->pipeline->execution == nullptr) {
                return LAPIS_E_INVALID_STATE;
            }
            pipeline = session->pipeline;
        }

        const auto result = pipeline->execution->set_fast_forward(
            lapis::media::PlaybackEpoch{expected_epoch.value},
            enabled != 0);
        std::scoped_lock lock(session->mutex);
        if (!matches_epoch(*session, expected_epoch)) {
            return LAPIS_E_STALE_EPOCH;
        }
        if (!result.ok()) {
            session->transport = LAPIS_TRANSPORT_FAULTED;
            return map_execution_error(result.error);
        }

        const auto snapshot = pipeline->execution->snapshot();
        session->position_ns = snapshot.clock.position.nanoseconds;
        return LAPIS_OK;
    });
}

lapis_result lapis_session_close(lapis_session* session, lapis_playback_epoch epoch) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);

        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            if (!advances_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }
            pipeline = session->pipeline;
        }

        if (pipeline != nullptr && pipeline->execution != nullptr) {
            const auto result = pipeline->execution->close(
                lapis::media::PlaybackEpoch{epoch.value});
            if (!result.ok()) {
                const auto mapped = map_execution_error(result.error);
                std::scoped_lock lock(session->mutex);
                session->epoch = epoch.value;
                session->transport = LAPIS_TRANSPORT_FAULTED;
                return mapped;
            }
        }

        std::shared_ptr<RuntimePlaybackPipeline> retired_pipeline;
        {
            std::scoped_lock lock(session->mutex);
            session->epoch = epoch.value;
            session->position_ns = 0;
            session->transport = LAPIS_TRANSPORT_CLOSED;
            retired_pipeline = std::move(session->pipeline);
        }
        retired_pipeline.reset();
        return LAPIS_OK;
    });
}

lapis_result lapis_session_set_video_output_surface(
    lapis_session* session,
    lapis_video_output_surface surface,
    lapis_playback_epoch epoch,
    lapis_video_output_surface_binding* out_binding) {
    return guarded_result([&]() -> lapis_result {
        if (session == nullptr || out_binding == nullptr) {
            return LAPIS_E_INVALID_ARGUMENT;
        }
        const auto descriptor = to_core_output_surface(surface);
        if (!descriptor.has_value()) {
            return LAPIS_E_INVALID_ARGUMENT;
        }

        std::scoped_lock command_lock(session->command_mutex);
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        lapis::media::VideoOutputSurfaceDescriptor previous{};
        {
            std::scoped_lock lock(session->mutex);
            if (!matches_epoch(*session, epoch)) {
                return LAPIS_E_STALE_EPOCH;
            }
            pipeline = session->pipeline;
            previous = session->desired_output_surface;
        }

        auto status = lapis::media::VideoOutputSurfaceControlStatus::Deferred;
        if (pipeline != nullptr && pipeline->execution != nullptr) {
            status = pipeline->execution->set_video_output_surface(*descriptor);
            if (status != lapis::media::VideoOutputSurfaceControlStatus::Applied &&
                status != lapis::media::VideoOutputSurfaceControlStatus::Deferred) {
                // A failed request is not committed as the next generation's
                // target. Restore the previous desired binding after the render
                // thread has acknowledged failure/teardown.
                (void)pipeline->execution->set_video_output_surface(previous);
                refresh_terminal_transport(session);
                return map_surface_control_error(status);
            }
        }

        {
            std::scoped_lock lock(session->mutex);
            session->desired_output_surface = *descriptor;
        }
        *out_binding = status == lapis::media::VideoOutputSurfaceControlStatus::Applied
            ? LAPIS_OUTPUT_SURFACE_APPLIED
            : LAPIS_OUTPUT_SURFACE_DEFERRED;
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

        lapis_playback_session_id session_id{};
        uint64_t epoch{};
        int64_t position_ns{};
        lapis_transport_state transport{};
        std::shared_ptr<RuntimePlaybackPipeline> pipeline;
        {
            std::scoped_lock lock(session->mutex);
            session_id = session->session_id;
            epoch = session->epoch;
            position_ns = session->position_ns;
            transport = session->transport;
            pipeline = session->pipeline;
        }

        if (pipeline != nullptr &&
            pipeline->execution != nullptr &&
            (active_transport(transport) || terminal_transport(transport))) {
            const auto execution_snapshot = pipeline->execution->snapshot();
            transport = transport_from_execution_state(
                execution_snapshot.state,
                transport);
            position_ns = execution_snapshot.clock.position.nanoseconds;
        }

        out_snapshot->session_id = session_id;
        out_snapshot->epoch.value = epoch;
        out_snapshot->position.nanoseconds = position_ns;
        out_snapshot->transport = transport;
        return LAPIS_OK;
    });
}
