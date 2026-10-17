#pragma once

#include "core/media_time.hpp"
#include "core/playback_epoch.hpp"

#include <cstdint>

namespace lapis::media {

enum class VideoScheduleAction : std::uint8_t {
    Wait,
    Present,
    Drop
};

enum class VideoDropReason : std::uint8_t {
    None,
    StaleEpoch,
    TooLate
};

struct VideoSchedulerConfig final {
    MediaTime early_tolerance{2'000'000};
    MediaTime late_drop_threshold{50'000'000};
};

struct VideoScheduleDecision final {
    VideoScheduleAction action{VideoScheduleAction::Present};
    VideoDropReason drop_reason{VideoDropReason::None};
    MediaTime frame_offset{};
    MediaTime wait_duration{};
};

class VideoScheduler final {
public:
    explicit VideoScheduler(VideoSchedulerConfig config = {});

    [[nodiscard]] VideoScheduleDecision decide(
        PlaybackEpoch sample_epoch,
        MediaTime frame_timestamp,
        PlaybackEpoch current_epoch,
        MediaTime clock_position) const noexcept;

private:
    VideoSchedulerConfig config_;
};

} // namespace lapis::media
