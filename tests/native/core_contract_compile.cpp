#include "core/platform_interfaces.hpp"
#include <bdmv/player_c_api.h>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<bdmv::core::MediaTime>);
static_assert(std::is_trivially_copyable_v<bdmv::core::FrameInfo>);
static_assert(sizeof(bdmv_session_snapshot) == 48);
static_assert(sizeof(bdmv_engine_capabilities) == 28);

int main() { return 0; }
