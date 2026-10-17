#include "core/playback_clock.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace lapis::media {

bool PlaybackClock::start(MediaTime position, MonotonicTime now) {
    if (position.nanoseconds < 0) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    if (state_ != PlaybackClockState::Stopped) {
        return false;
    }

    anchor_position_ = position;
    anchor_time_ = now;
    state_ = PlaybackClockState::Running;
    return true;
}

bool PlaybackClock::pause(MonotonicTime now) {
    std::scoped_lock lock(mutex_);
    if (state_ != PlaybackClockState::Running) {
        return false;
    }

    anchor_position_ = position_unlocked(now);
    anchor_time_ = now;
    state_ = PlaybackClockState::Paused;
    return true;
}

bool PlaybackClock::resume(MonotonicTime now) {
    std::scoped_lock lock(mutex_);
    if (state_ != PlaybackClockState::Paused) {
        return false;
    }

    anchor_time_ = now;
    state_ = PlaybackClockState::Running;
    return true;
}

bool PlaybackClock::seek(MediaTime position, MonotonicTime now) {
    if (position.nanoseconds < 0) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    anchor_position_ = position;
    anchor_time_ = now;
    return true;
}

bool PlaybackClock::stop(MonotonicTime now) {
    std::scoped_lock lock(mutex_);
    if (state_ == PlaybackClockState::Stopped) {
        return false;
    }

    anchor_position_ = position_unlocked(now);
    anchor_time_ = now;
    state_ = PlaybackClockState::Stopped;
    return true;
}

void PlaybackClock::reset(MonotonicTime now) {
    std::scoped_lock lock(mutex_);
    state_ = PlaybackClockState::Stopped;
    anchor_position_ = MediaTime{};
    anchor_time_ = now;
}

PlaybackClockSnapshot PlaybackClock::snapshot(MonotonicTime now) const {
    std::scoped_lock lock(mutex_);
    return {state_, position_unlocked(now)};
}

MediaTime PlaybackClock::position_unlocked(MonotonicTime now) const noexcept {
    if (state_ != PlaybackClockState::Running ||
        now.nanoseconds <= anchor_time_.nanoseconds) {
        return anchor_position_;
    }

    const auto elapsed = now.nanoseconds - anchor_time_.nanoseconds;
    constexpr auto max_position = std::numeric_limits<std::int64_t>::max();
    const auto headroom = static_cast<std::uint64_t>(
        max_position - anchor_position_.nanoseconds);
    const auto applied = std::min(elapsed, headroom);

    return MediaTime{
        anchor_position_.nanoseconds + static_cast<std::int64_t>(applied)
    };
}

} // namespace lapis::media
