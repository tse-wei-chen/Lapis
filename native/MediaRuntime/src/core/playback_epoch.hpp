#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace lapis::media {

class PlaybackEpoch final {
public:
    constexpr explicit PlaybackEpoch(std::uint64_t value = 0) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    [[nodiscard]] PlaybackEpoch next() const {
        if (value_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("playback epoch overflow");
        }
        return PlaybackEpoch(value_ + 1);
    }

private:
    std::uint64_t value_;
};

} // namespace lapis::media
