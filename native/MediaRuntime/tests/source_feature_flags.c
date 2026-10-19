#include "lapis/media_runtime.h"

#include <stdio.h>

int main(void) {
    const uint32_t features = lapis_runtime_get_source_features();
    if ((features & LAPIS_SOURCE_FEATURE_FILE) == 0u) {
        fputs("file provider feature missing\n", stderr);
        return 1;
    }
#if defined(LAPIS_ENABLE_LIBBLURAY)
    if ((features & LAPIS_SOURCE_FEATURE_BDMV_DIRECTORY) == 0u) {
        fputs("libbluray enabled but binary does not report BDMV provider\n", stderr);
        return 2;
    }
#else
    if ((features & LAPIS_SOURCE_FEATURE_BDMV_DIRECTORY) != 0u) {
        fputs("libbluray disabled but binary incorrectly reports BDMV\n", stderr);
        return 3;
    }
#endif
    if ((features & ~(LAPIS_SOURCE_FEATURE_FILE |
                      LAPIS_SOURCE_FEATURE_BDMV_DIRECTORY)) != 0u) {
        fputs("unexpected source capabilities\n", stderr);
        return 4;
    }
    return 0;
}
