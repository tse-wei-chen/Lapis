#pragma once

#include <stdint.h>

#if defined(_WIN32)
    #if defined(LAPIS_MEDIA_RUNTIME_EXPORTS)
        #define LAPIS_API __declspec(dllexport)
    #else
        #define LAPIS_API __declspec(dllimport)
    #endif
#else
    #define LAPIS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LAPIS_MEDIA_RUNTIME_ABI_VERSION 2u
/* Opt-in diagnostics extension; the existing playback ABI stays at v2. */
#define LAPIS_DIAGNOSTICS_API_VERSION 1u
/* Separate, optional read-only navigation extension; ABI v2 stays unchanged. */
#define LAPIS_NAVIGATION_API_VERSION 1u
/* Read-only embedded subtitle stream inventory; no subtitle decoding. */
#define LAPIS_SUBTITLE_TRACKS_API_VERSION 1u
#define LAPIS_SUBTITLE_TRACK_MAX 64u
#define LAPIS_DIAGNOSTIC_RING_CAPACITY 128u

/* Optional source-provider feature mask. This is build capability, not
 * proof that a particular BDMV disc is readable or decrypted. */
#define LAPIS_SOURCE_FEATURE_FILE 0x1u
#define LAPIS_SOURCE_FEATURE_BDMV_DIRECTORY 0x2u

typedef struct lapis_runtime lapis_runtime;
typedef struct lapis_session lapis_session;

typedef struct lapis_playback_session_id {
    uint8_t bytes[16];
} lapis_playback_session_id;

typedef struct lapis_media_time {
    int64_t nanoseconds;
} lapis_media_time;

typedef struct lapis_playback_epoch {
    uint64_t value;
} lapis_playback_epoch;

typedef enum lapis_media_locator_kind {
    LAPIS_LOCATOR_FILE = 0,
    LAPIS_LOCATOR_DIRECTORY = 1,
    LAPIS_LOCATOR_URI = 2,
    LAPIS_LOCATOR_OPTICAL_DISC = 3
} lapis_media_locator_kind;

/* value_utf8 is borrowed only for the duration of lapis_session_open(). */
typedef struct lapis_media_locator {
    lapis_media_locator_kind kind;
    const char* value_utf8;
} lapis_media_locator;

typedef enum lapis_result {
    LAPIS_OK = 0,
    LAPIS_E_INVALID_ARGUMENT = 1,
    LAPIS_E_INVALID_STATE = 2,
    LAPIS_E_OUT_OF_MEMORY = 3,
    LAPIS_E_STALE_EPOCH = 4,
    LAPIS_E_ABI_MISMATCH = 5,
    LAPIS_E_UNSUPPORTED = 6,
    LAPIS_E_OUTPUT_UNAVAILABLE = 7,
    LAPIS_E_BUFFER_TOO_SMALL = 8,
    LAPIS_E_INTERNAL = 100
} lapis_result;

typedef enum lapis_transport_state {
    LAPIS_TRANSPORT_CLOSED = 0,
    LAPIS_TRANSPORT_OPENING = 1,
    LAPIS_TRANSPORT_READY = 2,
    LAPIS_TRANSPORT_PLAYING = 3,
    LAPIS_TRANSPORT_PAUSED = 4,
    LAPIS_TRANSPORT_SEEKING = 5,
    LAPIS_TRANSPORT_ENDED = 6,
    LAPIS_TRANSPORT_FAULTED = 7
} lapis_transport_state;

typedef struct lapis_open_result {
    uint64_t capabilities;
    lapis_media_time duration;
    uint32_t has_duration;
} lapis_open_result;

typedef enum lapis_diagnostic_event_kind {
    LAPIS_DIAG_DEVICE_FAILURE = 1,
    LAPIS_DIAG_DEVICE_CREATED = 2,
    LAPIS_DIAG_SWAP_CHAIN_CREATED = 3,
    LAPIS_DIAG_SWAP_CHAIN_RESIZED = 4,
    LAPIS_DIAG_RECOVERY_ATTEMPTED = 5,
    LAPIS_DIAG_RECOVERY_BIND_SUCCEEDED = 6,
    LAPIS_DIAG_RECOVERY_BIND_FAILED = 7,
    LAPIS_DIAG_RECOVERY_PRESENTED = 8,
    LAPIS_DIAG_RECOVERY_TERMINATED = 9,
    LAPIS_DIAG_RECOVERY_INCOMPLETE_AT_EOF = 10
} lapis_diagnostic_event_kind;

/* Fixed-width 48-byte wire record, no pointers, paths or owned memory. */
typedef struct lapis_diagnostic_event {
    uint64_t sequence;
    uint64_t epoch;
    uint64_t monotonic_milliseconds;
    uint32_t kind;
    uint32_t failure_stage;
    uint32_t hresult;
    uint32_t removed_reason;
    uint64_t detail;
} lapis_diagnostic_event;

/* Stable, fixed-width, pointer-free descriptor. Encoded as UTF-8, NUL
 * terminated and truncated at a codepoint boundary when necessary.
 * Stream indexes are FFmpeg container stream indexes, NOT an ordinal within
 * this returned list. Bitmap tracks (PGS/DVD) are metadata only. */
typedef enum lapis_subtitle_track_kind {
    LAPIS_SUBTITLE_TRACK_UNKNOWN = 0,
    LAPIS_SUBTITLE_TRACK_TEXT = 1,
    LAPIS_SUBTITLE_TRACK_BITMAP = 2
} lapis_subtitle_track_kind;

typedef struct lapis_subtitle_track_info {
    uint32_t stream_index;
    uint32_t kind;
    char language_utf8[24];
    char codec_name_utf8[40];
} lapis_subtitle_track_info;

typedef struct lapis_playback_snapshot {
    lapis_playback_session_id session_id;
    lapis_playback_epoch epoch;
    lapis_media_time position;
    lapis_transport_state transport;
} lapis_playback_snapshot;

/*
 * Output-surface binding is independent of playback discontinuities. None must
 * have a zero native_handle; Win32Window must have a non-zero HWND represented
 * as uintptr_t. The caller owns the HWND and must keep it alive until a later
 * successful detach/replacement, session close, or session destruction.
 * A successful call returns APPLIED if acknowledged by the render thread,
 * otherwise DEFERRED when no render generation is active. Both states guarantee
 * the previously attached window is no longer accessed after the call returns.
 * The requested epoch must match the session's current playback epoch; binding
 * does not itself increment that epoch. A failed binding is not committed.
 */
typedef enum lapis_video_output_surface_kind {
    LAPIS_VIDEO_OUTPUT_SURFACE_NONE = 0,
    LAPIS_VIDEO_OUTPUT_SURFACE_WIN32_WINDOW = 1
} lapis_video_output_surface_kind;

typedef struct lapis_video_output_surface {
    lapis_video_output_surface_kind kind;
    uintptr_t native_handle;
} lapis_video_output_surface;

typedef enum lapis_video_output_surface_binding {
    LAPIS_OUTPUT_SURFACE_APPLIED = 0,
    LAPIS_OUTPUT_SURFACE_DEFERRED = 1
} lapis_video_output_surface_binding;

/*
 * Lifetime contract:
 * - a runtime must outlive all sessions created from it;
 * - result-returning session operations may be called concurrently;
 * - lapis_session_destroy requires that no operation is in flight for that session;
 * - lapis_runtime_destroy requires that all child sessions have already been destroyed.
 */
LAPIS_API uint32_t lapis_runtime_get_abi_version(void);

/* ABI-v2-compatible read-only extension; an older DLL without the export
 * means UNKNOWN capability. No session or runtime initialization required. */
LAPIS_API uint32_t lapis_runtime_get_source_features(void);

/* Optional diagnostics v1 extension, discover before use with dlsym/GetProcAddress.
 * Read an ordered bounded journal. since_sequence is exclusive. If the reader
 * falls behind the 128-event ring, out_dropped reports overwritten events.
 * out_next_sequence is the sequence of the last *returned* event (not latest
 * produced); call again with that cursor to page without silent skipping.
 * A zero-capacity probe is valid with events=NULL. The operation is observational
 * and does not mutate playback epoch, transport, or the journal.
 */
LAPIS_API uint32_t lapis_runtime_get_diagnostics_api_version(void);
LAPIS_API lapis_result lapis_session_read_diagnostic_events(
    lapis_session* session,
    uint64_t since_sequence,
    lapis_diagnostic_event* events,
    uint32_t capacity,
    uint32_t* out_count,
    uint64_t* out_next_sequence,
    uint64_t* out_dropped);

/*
 * Optional navigation v1 extension (discover with GetProcAddress).
 * Resolve the source-selected title's chapter ID to a canonical time.
 * This is strictly read-only: call lapis_session_seek() at a NEW epoch to
 * retire old audio/video/packet workers. Only READY/PLAYING/PAUSED are legal.
 * The expected epoch must match the current session; a closed/file source
 * returns UNSUPPORTED, an unknown chapter ID returns INVALID_ARGUMENT.
 * The output is zeroed on every error. No BD-J or menu action occurs.
 */
LAPIS_API uint32_t lapis_runtime_get_navigation_api_version(void);

/* Optional metadata extension: look up version export at runtime before
 * calling either entrypoint when supporting older Lapis DLLs.
 *
 * Probe: tracks=NULL, capacity=0; out_count returns total tracks (<=64).
 * If capacity < total, returns BUFFER_TOO_SMALL and leaves tracks untouched.
 * On invalid/stale/closed requests, *out_count is set to zero.
 * Uses the same command/epoch fence as Open/Seek/Close, but NEVER decodes.
 * Enumerated tracks are not automatically renderable/selected.
 */
LAPIS_API uint32_t lapis_runtime_get_subtitle_tracks_api_version(void);
LAPIS_API lapis_result lapis_session_list_subtitle_tracks(
    lapis_session* session,
    lapis_playback_epoch expected_epoch,
    lapis_subtitle_track_info* tracks,
    uint32_t capacity,
    uint32_t* out_count);
LAPIS_API lapis_result lapis_session_resolve_chapter(
    lapis_session* session,
    uint32_t chapter_id,
    lapis_playback_epoch expected_epoch,
    lapis_media_time* out_position);

LAPIS_API lapis_result lapis_runtime_create(lapis_runtime** out_runtime);
LAPIS_API void lapis_runtime_destroy(lapis_runtime* runtime);

LAPIS_API lapis_result lapis_session_create(
    lapis_runtime* runtime,
    lapis_playback_session_id session_id,
    lapis_session** out_session);
LAPIS_API void lapis_session_destroy(lapis_session* session);

LAPIS_API lapis_result lapis_session_open(
    lapis_session* session,
    const lapis_media_locator* locator,
    lapis_playback_epoch epoch,
    lapis_open_result* out_result);
LAPIS_API lapis_result lapis_session_play(
    lapis_session* session,
    lapis_playback_epoch epoch);
LAPIS_API lapis_result lapis_session_pause(
    lapis_session* session,
    lapis_playback_epoch epoch);
LAPIS_API lapis_result lapis_session_seek(
    lapis_session* session,
    lapis_media_time position,
    lapis_playback_epoch epoch);
LAPIS_API lapis_result lapis_session_close(
    lapis_session* session,
    lapis_playback_epoch epoch);
LAPIS_API lapis_result lapis_session_set_video_output_surface(
    lapis_session* session,
    lapis_video_output_surface surface,
    lapis_playback_epoch epoch,
    lapis_video_output_surface_binding* out_binding);
LAPIS_API lapis_result lapis_session_get_snapshot(
    lapis_session* session,
    lapis_playback_snapshot* out_snapshot);

#ifdef __cplusplus
}
#endif
