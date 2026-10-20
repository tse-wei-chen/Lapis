#ifndef BDMV_PLAYER_C_API_H
#define BDMV_PLAYER_C_API_H

#include <stdint.h>

#if defined(_WIN32)
  #if defined(BDMV_RUNTIME_EXPORTS)
    #define BDMV_API __declspec(dllexport)
  #else
    #define BDMV_API __declspec(dllimport)
  #endif
  #define BDMV_CALL __cdecl
#else
  #define BDMV_API __attribute__((visibility("default")))
  #define BDMV_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* v0.2: all public structs begin with struct_size, fixed-width fields only.
 * Every function returns status and must not throw exceptions across this ABI.
 * Caller owns engine/session; explicit lifetime APIs below. */
#define BDMV_ABI_MAJOR 1u
#define BDMV_ABI_MINOR 1u
#define BDMV_ABI_PATCH 0u

typedef struct bdmv_engine bdmv_engine;
typedef struct bdmv_session bdmv_session;

typedef enum bdmv_status {
    BDMV_OK = 0,
    BDMV_INVALID_ARGUMENT = 1,
    BDMV_ABI_MISMATCH = 2,
    BDMV_OUT_OF_MEMORY = 3,
    BDMV_INVALID_STATE = 4,
    BDMV_UNSUPPORTED = 5,
    BDMV_INTERNAL_ERROR = 6,
    BDMV_AGAIN = 7,
    BDMV_BUFFER_TOO_SMALL = 8,
    BDMV_END_OF_STREAM = 9,
    BDMV_DECODE_ERROR = 10
} bdmv_status;

typedef enum bdmv_transport {
    BDMV_CLOSED = 0,
    BDMV_OPENING = 1,
    BDMV_READY = 2,
    BDMV_PLAYING = 3,
    BDMV_PAUSED = 4,
    BDMV_SEEKING = 5,
    BDMV_ENDED = 6,
    BDMV_FAULTED = 7
} bdmv_transport;

typedef enum bdmv_video_backend {
    BDMV_VIDEO_AUTO = 0,
    BDMV_VIDEO_VULKAN = 1,
    BDMV_VIDEO_D3D11 = 2,
    BDMV_VIDEO_OPENGL = 3,
    BDMV_VIDEO_SOFTWARE = 4
} bdmv_video_backend;

typedef enum bdmv_audio_backend {
    BDMV_AUDIO_AUTO = 0,
    BDMV_AUDIO_WASAPI = 1,
    BDMV_AUDIO_PIPEWIRE = 2,
    BDMV_AUDIO_ALSA = 3,
    BDMV_AUDIO_NULL = 4
} bdmv_audio_backend;

typedef struct bdmv_abi_version {
    uint32_t struct_size;
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} bdmv_abi_version;

typedef struct bdmv_engine_config {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t preferred_video_backend;
    uint32_t preferred_audio_backend;
} bdmv_engine_config;

typedef struct bdmv_engine_capabilities {
    uint32_t struct_size;
    uint32_t test_source;  /* 1 only if compiled with diagnostic source */
    uint32_t file_source;  /* 0 until implemented */
    uint32_t bluray_source;/* 0 until implemented */
    uint32_t video_output; /* 0 until implemented */
    uint32_t audio_output; /* 0 until implemented */
    uint32_t reserved;
} bdmv_engine_capabilities;

/* ABI minor 1: caller-owned CPU decode buffers (not actual screen/audio output).
 * A too-small buffer returns BDMV_BUFFER_TOO_SMALL without consuming the frame.
 * BDMV_AGAIN is normal for nonblocking reads. No frame memory is borrowed. */
typedef enum bdmv_decoded_kind {
    BDMV_DECODED_RGBA8 = 0,
    BDMV_DECODED_PCM_S16_STEREO = 1
} bdmv_decoded_kind;

typedef struct bdmv_decoded_frame_info {
    uint32_t struct_size;
    uint32_t kind;
    uint64_t session_id;
    uint64_t epoch;
    int64_t pts_ns;
    int64_t duration_ns;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t required_bytes;
} bdmv_decoded_frame_info;

typedef struct bdmv_session_snapshot {
    uint32_t struct_size;
    uint32_t transport;
    uint64_t session_id;
    uint64_t epoch;
    int64_t position_ns;
    uint32_t navigation;
    uint32_t output;
    uint32_t ai;
    uint32_t reserved;
} bdmv_session_snapshot;

/* No heap-allocated result strings cross the boundary in v0.2.
 * All pointers must be valid; passing NULL to destroy is safe.
 * Caller's responsibility: do not destroy a session concurrently with calls
 * to that same session. Other session commands are mutex-serialized. */
BDMV_API bdmv_status BDMV_CALL bdmv_get_abi_version(bdmv_abi_version* out_version);
BDMV_API bdmv_status BDMV_CALL bdmv_engine_create(const bdmv_engine_config* config,
                                                    bdmv_engine** out_engine);
BDMV_API void BDMV_CALL bdmv_engine_destroy(bdmv_engine* engine);
BDMV_API bdmv_status BDMV_CALL bdmv_engine_query_capabilities(const bdmv_engine* engine,
                                                              bdmv_engine_capabilities* out_caps);
BDMV_API bdmv_status BDMV_CALL bdmv_session_create(const bdmv_engine* engine,
                                                   bdmv_session** out_session);
BDMV_API void BDMV_CALL bdmv_session_destroy(bdmv_session* session);
/* Diagnostic URI "test://synthetic" is state-only.
 * "test://frames" yields finite fake RGBA/PCM only when test sources are enabled.
 * Local file paths/file:// URIs work only when built with FFmpeg dev libraries.
 * Remote URLs, discs and protected content remain UNSUPPORTED. */
BDMV_API bdmv_status BDMV_CALL bdmv_session_open_uri(bdmv_session* session, const char* utf8_uri);
BDMV_API bdmv_status BDMV_CALL bdmv_session_play(bdmv_session* session);
BDMV_API bdmv_status BDMV_CALL bdmv_session_pause(bdmv_session* session);
BDMV_API bdmv_status BDMV_CALL bdmv_session_seek(bdmv_session* session, int64_t position_ns);
BDMV_API bdmv_status BDMV_CALL bdmv_session_close(bdmv_session* session);
/* Nonblocking, caller-owned, two-call read; buffer may be NULL to query size.
 * A frame remains pending across BUFFER_TOO_SMALL; seek/close discards it.
 * Do not call concurrently with session destruction. */
BDMV_API bdmv_status BDMV_CALL bdmv_session_read_frame(bdmv_session* session,
                                                        bdmv_decoded_frame_info* out_info,
                                                        uint8_t* buffer,
                                                        uint64_t buffer_capacity);
BDMV_API bdmv_status BDMV_CALL bdmv_session_get_snapshot(const bdmv_session* session,
                                                         bdmv_session_snapshot* out_snapshot);

#ifdef __cplusplus
}
#endif
#endif
