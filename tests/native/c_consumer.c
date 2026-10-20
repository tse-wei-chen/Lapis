#include <bdmv/player_c_api.h>
#include <stdint.h>
#include <stdio.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

/* Verifies that public header is really consumable by a C compiler, not just C++. */
int main(void) {
    assert(sizeof(bdmv_abi_version) == 16);
    assert(sizeof(bdmv_engine_config) == 16);
    assert(sizeof(bdmv_engine_capabilities) == 28);
    assert(sizeof(bdmv_session_snapshot) == 48);
    assert(sizeof(bdmv_decoded_frame_info) == 64);
    bdmv_abi_version v = {sizeof(bdmv_abi_version), 0, 0, 0};
    assert(bdmv_get_abi_version(&v) == BDMV_OK);
    assert(v.major == BDMV_ABI_MAJOR);
    puts("pure C consumer + layout passed");
    return 0;
}
