#include "core/playback_clock.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace lapis::media {

bool PlaybackClock::start(MediaTime position, MonotonicTime now) {
    if (position.nanoseconds < 0) {
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        if (state_ != PlaybackClockState::Stopped) {
            return false;
        }

        anchor_position_ = position;
        anchor_time_ = now;
        state_ = PlaybackClockState::Running;
        changed_unlocked();
    }
    changed_.notify_all();
    return true;
}

bool PlaybackClock::pause(MonotonicTime now) {
    {
        std::scoped_lock lock(mutex_);
        if (state_ != PlaybackClockState::Running) {
            return false;
        }

        anchor_position_ = position_unlocked(now);
        anchor_time_ = now;
        state_ = PlaybackClockState::Paused;
        changed_unlocked();
    }
    changed_.notify_all();
    return true;
}

bool PlaybackClock::resume(MonotonicTime now) {
    {
        std::scoped_lock lock(mutex_);
        if (state_ != PlaybackClockState::Paused) {
            return false;
        }

        anchor_time_ = now;
        state_ = PlaybackClockState::Running;
        changed_unlocked();
    }
    changed_.notify_all();
    return true;
}

bool PlaybackClock::rebase(
    PlaybackEpoch epoch,
    MediaTime position,
    MonotonicTime now) {
    if (position.nanoseconds < 0) {
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        epoch_ = epoch;
        anchor_position_ = position;
        anchor_time_ = now;
        changed_unlocked();
    }
    changed_.notify_all();
    return true;
}

bool PlaybackClock::stop(MonotonicTime now) {
    {
        std::scoped_lock lock(mutex_);
        if (state_ == PlaybackClockState::Stopped) {
            return false;
        }

        anchor_position_ = position_unlocked(now);
        anchor_time_ = now;
        state_ = PlaybackClockState::Stopped;
        changed_unlocked();
    }
    changed_.notify_all();
    return true;
}

void PlaybackClock::reset(PlaybackEpoch epoch, MonotonicTime now) {
    {
        std::scoped_lock lock(mutex_);
        state_ = PlaybackClockState::Stopped;
        epoch_ = epoch;
        anchor_position_ = MediaTime{};
        anchor_time_ = now;
        changed_unlocked();
    }
    changed_.notify_all();
}

PlaybackClockSnapshot PlaybackClock::snapshot(MonotonicTime now) const {
    std::scoped_lock lock(mutex_);
    return {state_, epoch_, position_unlocked(now), revision_};
}

PlaybackClockWaitStatus PlaybackClock::wait_for_change(
    std::uint64_t observed_revision,
    std::stop_token stop_token) const {
    std::unique_lock lock(mutex_);
    if (revision_ != observed_revision) {
        return PlaybackClockWaitStatus::Changed;
    }
    if (stop_token.stop_requested()) {
        return PlaybackClockWaitStatus::Stopped;
    }

    const bool changed = changed_.wait(
        lock,
        stop_token,
        [this, observed_revision] {
            return revision_ != observed_revision;
        });

    if (revision_ != observed_revision || changed) {
        return PlaybackClockWaitStatus::Changed;
    }
    return PlaybackClockWaitStatus::Stopped;
}

PlaybackClockWaitStatus PlaybackClock::wait_for_change_for(
    std::uint64_t observed_revision,
    std::chrono::nanoseconds maximum_wait,
    std::stop_token stop_token) const {
    std::unique_lock lock(mutex_);
    if (revision_ != observed_revision) {
        return PlaybackClockWaitStatus::Changed;
    }
    if (stop_token.stop_requested()) {
        return PlaybackClockWaitStatus::Stopped;
    }
    if (maximum_wait <= std::chrono::nanoseconds::zero()) {
        return PlaybackClockWaitStatus::Timeout;
    }

    const bool changed = changed_.wait_for(
        lock,
        stop_token,
        maximum_wait,
        [this, observed_revision] {
            return revision_ != observed_revision;
        });

    if (revision_ != observed_revision || changed) {
        return PlaybackClockWaitStatus::Changed;
    }
    if (stop_token.stop_requested()) {
        return PlaybackClockWaitStatus::Stopped;
    }
    return PlaybackClockWaitStatus::Timeout;
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

void PlaybackClock::changed_unlocked() noexcept {
    ++revision_;
}

} // namespace lapis::media
