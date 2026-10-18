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
