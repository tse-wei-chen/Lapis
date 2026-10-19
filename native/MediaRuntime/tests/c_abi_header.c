#include "lapis/media_runtime.h"

#include <stdint.h>
#include <stddef.h>

_Static_assert(sizeof(lapis_playback_session_id) == 16, "session id ABI must remain 128-bit");
_Static_assert(sizeof(lapis_media_time) == sizeof(int64_t), "media time ABI changed");
_Static_assert(sizeof(lapis_playback_epoch) == sizeof(uint64_t), "epoch ABI changed");
_Static_assert(sizeof(lapis_media_time) == 8, "chapter resolution timestamp ABI changed");
_Static_assert(sizeof(lapis_video_output_surface_kind) == sizeof(int), "output surface enum ABI changed");
_Static_assert(offsetof(lapis_video_output_surface, native_handle) % sizeof(uintptr_t) == 0, "output handle alignment changed");
_Static_assert(sizeof(lapis_video_output_surface) == offsetof(lapis_video_output_surface, native_handle) + sizeof(uintptr_t), "output surface layout changed");

int main(void) {
    lapis_media_locator locator = {LAPIS_LOCATOR_FILE, "sample.mkv"};
    lapis_open_result open_result = {0};
    lapis_playback_snapshot snapshot = {0};
    lapis_video_output_surface surface = {LAPIS_VIDEO_OUTPUT_SURFACE_NONE, 0};
    lapis_video_output_surface_binding binding = LAPIS_OUTPUT_SURFACE_DEFERRED;

    (void)locator;
    (void)open_result;
    (void)snapshot;
    (void)surface;
    (void)binding;
    return LAPIS_MEDIA_RUNTIME_ABI_VERSION == 2u ? 0 : 1;
}
