#pragma once

#include "core/media_time.hpp"
#include "core/monotonic_time.hpp"
#include "core/playback_epoch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace lapis::media {

enum class PlaybackClockState : std::uint8_t {
    Stopped,
    Running,
    Paused
};

enum class PlaybackClockWaitStatus : std::uint8_t {
    Changed,
    Timeout,
    Stopped
};

struct PlaybackClockSnapshot final {
    PlaybackClockState state{PlaybackClockState::Stopped};
    PlaybackEpoch epoch{};
    MediaTime position{};
    std::uint64_t revision{};
};

class PlaybackClock final {
public:
    [[nodiscard]] bool start(MediaTime position, MonotonicTime now);
    [[nodiscard]] bool pause(MonotonicTime now);
    [[nodiscard]] bool resume(MonotonicTime now);
    [[nodiscard]] bool rebase(
        PlaybackEpoch epoch,
        MediaTime position,
        MonotonicTime now);

    // Compare-and-apply correction for asynchronous clock-discipline owners.
    // The mutation succeeds only when state, epoch, and revision still match the
    // snapshot that produced the correction. This prevents a stale hardware
    // observation from rebasing across Pause/Resume/Seek transitions.
    [[nodiscard]] bool try_rebase(
        const PlaybackClockSnapshot& expected,
        MediaTime position,
        MonotonicTime now);

    [[nodiscard]] bool stop(MonotonicTime now);
    void reset(PlaybackEpoch epoch, MonotonicTime now);

    [[nodiscard]] PlaybackClockSnapshot snapshot(MonotonicTime now) const;

    [[nodiscard]] PlaybackClockWaitStatus wait_for_change(
        std::uint64_t observed_revision,
        std::stop_token stop_token = {}) const;

    [[nodiscard]] PlaybackClockWaitStatus wait_for_change_for(
        std::uint64_t observed_revision,
        std::chrono::nanoseconds maximum_wait,
        std::stop_token stop_token = {}) const;

private:
    [[nodiscard]] MediaTime position_unlocked(MonotonicTime now) const noexcept;
    void changed_unlocked() noexcept;

    mutable std::mutex mutex_;
    mutable std::condition_variable_any changed_;
    PlaybackClockState state_{PlaybackClockState::Stopped};
    PlaybackEpoch epoch_{};
    MediaTime anchor_position_{};
    MonotonicTime anchor_time_{};
    std::uint64_t revision_{};
};

} // namespace lapis::media
