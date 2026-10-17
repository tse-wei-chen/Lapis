#include "video/video_scheduler.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace lapis::media {
namespace {

std::int64_t saturating_subtract(
    std::int64_t left,
    std::int64_t right) noexcept {
    constexpr auto min_value = std::numeric_limits<std::int64_t>::min();
    constexpr auto max_value = std::numeric_limits<std::int64_t>::max();

    if (right > 0 && left < min_value + right) {
        return min_value;
    }
    if (right < 0 && left > max_value + right) {
        return max_value;
    }
    return left - right;
}

} // namespace

VideoScheduler::VideoScheduler(VideoSchedulerConfig config)
    : config_(config) {
    if (config_.early_tolerance.nanoseconds < 0 ||
        config_.late_drop_threshold.nanoseconds < 0) {
        throw std::invalid_argument(
            "Video scheduler tolerances cannot be negative.");
    }
}

VideoScheduleDecision VideoScheduler::decide(
    PlaybackEpoch sample_epoch,
    MediaTime frame_timestamp,
    PlaybackEpoch current_epoch,
    MediaTime clock_position) const noexcept {
    const MediaTime offset{
        saturating_subtract(
            frame_timestamp.nanoseconds,
            clock_position.nanoseconds)
    };

    if (sample_epoch.value() != current_epoch.value()) {
        return {
            VideoScheduleAction::Drop,
            VideoDropReason::StaleEpoch,
            offset,
            MediaTime{}
        };
    }

    if (offset.nanoseconds > config_.early_tolerance.nanoseconds) {
        return {
            VideoScheduleAction::Wait,
            VideoDropReason::None,
            offset,
            MediaTime{
                offset.nanoseconds - config_.early_tolerance.nanoseconds
            }
        };
    }

    if (offset.nanoseconds < -config_.late_drop_threshold.nanoseconds) {
        return {
            VideoScheduleAction::Drop,
            VideoDropReason::TooLate,
            offset,
            MediaTime{}
        };
    }

    return {
        VideoScheduleAction::Present,
        VideoDropReason::None,
        offset,
        MediaTime{}
    };
}

} // namespace lapis::media
