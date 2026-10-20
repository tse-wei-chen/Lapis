#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

namespace bdmv::core {

// Pure arithmetic: caller supplies steady-clock timestamps (nanoseconds).
// Future audio clock adapter calls reanchor() on sink handoff; no OS clock in Core.
class PlaybackClock final {
public:
    void reanchor(std::int64_t media_ns, std::int64_t monotonic_ns) noexcept {
        std::lock_guard lock(mutex_);
        origin_media_ns_ = std::max<std::int64_t>(0, media_ns);
        origin_wall_ns_ = monotonic_ns;
    }

    void play(std::int64_t monotonic_ns) noexcept {
        std::lock_guard lock(mutex_);
        if (!playing_) {
            origin_wall_ns_ = monotonic_ns;
            playing_ = true;
        }
    }

    void pause(std::int64_t monotonic_ns) noexcept {
        std::lock_guard lock(mutex_);
        if (playing_) {
            origin_media_ns_ = position_unlocked(monotonic_ns);
            playing_ = false;
        }
    }

    void seek(std::int64_t media_ns, std::int64_t monotonic_ns) noexcept {
        reanchor(media_ns, monotonic_ns);
    }

    [[nodiscard]] std::int64_t position(std::int64_t monotonic_ns) const noexcept {
        std::lock_guard lock(mutex_);
        return position_unlocked(monotonic_ns);
    }

    [[nodiscard]] bool is_playing() const noexcept {
        std::lock_guard lock(mutex_);
        return playing_;
    }

private:
    [[nodiscard]] std::int64_t position_unlocked(std::int64_t now) const noexcept {
        if (!playing_ || now <= origin_wall_ns_) return origin_media_ns_;
        // Subtraction without overflow, even for untrusted time-provider inputs.
        const auto elapsed = static_cast<std::uint64_t>(now) -
                             static_cast<std::uint64_t>(origin_wall_ns_);
        const auto remaining = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - origin_media_ns_);
        if (elapsed >= remaining) return std::numeric_limits<std::int64_t>::max();
        return origin_media_ns_ + static_cast<std::int64_t>(elapsed);
    }

    mutable std::mutex mutex_;
    std::int64_t origin_media_ns_{0};
    std::int64_t origin_wall_ns_{0};
    bool playing_{false};
};

} // namespace bdmv::core
