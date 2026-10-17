#pragma once

#include <chrono>
#include <compare>
#include <cstdint>

namespace lapis::media {

struct MonotonicTime final {
    std::uint64_t nanoseconds{};

    auto operator<=>(const MonotonicTime&) const = default;
};

class SteadyClockSource final {
public:
    [[nodiscard]] static MonotonicTime now() noexcept {
        const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return MonotonicTime{
            value > 0 ? static_cast<std::uint64_t>(value) : 0U
        };
    }
};

} // namespace lapis::media
