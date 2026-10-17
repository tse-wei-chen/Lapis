#include "lapis/media_runtime.h"

#include <stdint.h>

_Static_assert(sizeof(lapis_playback_session_id) == 16, "session id ABI must remain 128-bit");
_Static_assert(sizeof(lapis_media_time) == sizeof(int64_t), "media time ABI changed");
_Static_assert(sizeof(lapis_playback_epoch) == sizeof(uint64_t), "epoch ABI changed");

int main(void) {
    lapis_media_locator locator = {LAPIS_LOCATOR_FILE, "sample.mkv"};
    lapis_open_result open_result = {0};
    lapis_playback_snapshot snapshot = {0};

    (void)locator;
    (void)open_result;
    (void)snapshot;
    return LAPIS_MEDIA_RUNTIME_ABI_VERSION == 1u ? 0 : 1;
}
