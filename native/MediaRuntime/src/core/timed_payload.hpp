#pragma once

#include "core/media_time.hpp"
#include "core/playback_epoch.hpp"

namespace lapis::media {

template <typename T>
struct TimedPayload final {
    PlaybackEpoch epoch;
    MediaTime timestamp;
    T payload;
};

} // namespace lapis::media
