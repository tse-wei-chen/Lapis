#include <bdmv/player_c_api.h>
#include "core/session.hpp"
#include <atomic>
#include <new>
#include <string_view>

struct bdmv_engine {
    std::uint32_t preferred_video_backend;
    std::uint32_t preferred_audio_backend;
};

struct bdmv_session {
    explicit bdmv_session(std::uint64_t id) noexcept : impl(id) {}
    bdmv::core::Session impl;
};

namespace {
std::atomic<std::uint64_t> next_session_id{1};
// Writes only the known fields; callers must initialize struct_size.
template <typename T>
bool valid(const T* value) noexcept { return value && value->struct_size >= sizeof(T); }
}

extern "C" {
BDMV_API bdmv_status BDMV_CALL bdmv_get_abi_version(bdmv_abi_version* out) {
    if (!valid(out)) return BDMV_ABI_MISMATCH;
    *out = {sizeof(bdmv_abi_version), BDMV_ABI_MAJOR, BDMV_ABI_MINOR, BDMV_ABI_PATCH};
    return BDMV_OK;
}
BDMV_API bdmv_status BDMV_CALL bdmv_engine_create(const bdmv_engine_config* config,
                                                   bdmv_engine** out) {
    if (!out) return BDMV_INVALID_ARGUMENT;
    *out = nullptr;
    if (!valid(config)) return BDMV_ABI_MISMATCH;
    if (config->flags != 0 || config->preferred_video_backend > BDMV_VIDEO_SOFTWARE ||
        config->preferred_audio_backend > BDMV_AUDIO_NULL) return BDMV_INVALID_ARGUMENT;
    auto* value = new (std::nothrow) bdmv_engine{config->preferred_video_backend,
                                                 config->preferred_audio_backend};
    if (!value) return BDMV_OUT_OF_MEMORY;
    *out = value;
    return BDMV_OK;
}
BDMV_API void BDMV_CALL bdmv_engine_destroy(bdmv_engine* engine) { delete engine; }
BDMV_API bdmv_status BDMV_CALL bdmv_engine_query_capabilities(const bdmv_engine* engine,
                                                               bdmv_engine_capabilities* out) {
    if (!engine) return BDMV_INVALID_ARGUMENT;
    if (!valid(out)) return BDMV_ABI_MISMATCH;
#ifdef BDMV_ENABLE_TEST_SOURCE
    constexpr std::uint32_t test_source = 1;
#else
    constexpr std::uint32_t test_source = 0;
#endif
#ifdef BDMV_WITH_FFMPEG
    constexpr std::uint32_t file_source = 1;
#else
    constexpr std::uint32_t file_source = 0;
#endif
    *out = {sizeof(bdmv_engine_capabilities), test_source, file_source, 0, 0, 0, 0};
    return BDMV_OK;
}
BDMV_API bdmv_status BDMV_CALL bdmv_session_create(const bdmv_engine* engine,
                                                    bdmv_session** out) {
    if (!out) return BDMV_INVALID_ARGUMENT;
    *out = nullptr;
    if (!engine) return BDMV_INVALID_ARGUMENT;
    auto* value = new (std::nothrow) bdmv_session(next_session_id.fetch_add(1));
    if (!value) return BDMV_OUT_OF_MEMORY;
    *out = value;
    return BDMV_OK;
}
BDMV_API void BDMV_CALL bdmv_session_destroy(bdmv_session* session) { delete session; }
BDMV_API bdmv_status BDMV_CALL bdmv_session_open_uri(bdmv_session* session, const char* uri) {
    if (!session || !uri) return BDMV_INVALID_ARGUMENT;
    return session->impl.open(std::string_view(uri));
}
BDMV_API bdmv_status BDMV_CALL bdmv_session_play(bdmv_session* session) {
    return session ? session->impl.play() : BDMV_INVALID_ARGUMENT;
}
BDMV_API bdmv_status BDMV_CALL bdmv_session_pause(bdmv_session* session) {
    return session ? session->impl.pause() : BDMV_INVALID_ARGUMENT;
}
BDMV_API bdmv_status BDMV_CALL bdmv_session_seek(bdmv_session* session, int64_t position_ns) {
    return session ? session->impl.seek(position_ns) : BDMV_INVALID_ARGUMENT;
}
BDMV_API bdmv_status BDMV_CALL bdmv_session_close(bdmv_session* session) {
    return session ? session->impl.close() : BDMV_INVALID_ARGUMENT;
}
BDMV_API bdmv_status BDMV_CALL bdmv_session_read_frame(bdmv_session* session,
                                                        bdmv_decoded_frame_info* info,
                                                        uint8_t* buffer, uint64_t capacity) {
    return session ? session->impl.read_frame(info, buffer, capacity) : BDMV_INVALID_ARGUMENT;
}
BDMV_API bdmv_status BDMV_CALL bdmv_session_get_snapshot(const bdmv_session* session,
                                                          bdmv_session_snapshot* out) {
    if (!session) return BDMV_INVALID_ARGUMENT;
    if (!valid(out)) return BDMV_ABI_MISMATCH;
    *out = session->impl.snapshot();
    return BDMV_OK;
}
} // extern "C"
