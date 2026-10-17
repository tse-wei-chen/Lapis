#pragma once

#include "core/media_time.hpp"
#include "core/monotonic_time.hpp"

#include <cstdint>
#include <mutex>

namespace lapis::media {

enum class PlaybackClockState : std::uint8_t {
    Stopped,
    Running,
    Paused
};

struct PlaybackClockSnapshot final {
    PlaybackClockState state{PlaybackClockState::Stopped};
    MediaTime position{};
};

class PlaybackClock final {
public:
    [[nodiscard]] bool start(MediaTime position, MonotonicTime now);
    [[nodiscard]] bool pause(MonotonicTime now);
    [[nodiscard]] bool resume(MonotonicTime now);
    [[nodiscard]] bool seek(MediaTime position, MonotonicTime now);
    [[nodiscard]] bool stop(MonotonicTime now);
    void reset(MonotonicTime now);

    [[nodiscard]] PlaybackClockSnapshot snapshot(MonotonicTime now) const;

private:
    [[nodiscard]] MediaTime position_unlocked(MonotonicTime now) const noexcept;

    mutable std::mutex mutex_;
    PlaybackClockState state_{PlaybackClockState::Stopped};
    MediaTime anchor_position_{};
    MonotonicTime anchor_time_{};
};

} // namespace lapis::media
