#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace bdmv::core {

// No floating-point accumulation: rational media timestamps (e.g. 1/90000)
// are converted once to ns. Inputs not representable in int64 are rejected.
inline std::optional<std::int64_t> timebase_to_ns(std::int64_t ticks,
                                                   std::int64_t numerator,
                                                   std::int64_t denominator) noexcept {
    if (numerator <= 0 || denominator <= 0) return std::nullopt;
    constexpr auto ns_per_second = std::int64_t{1000000000};
    const auto max = std::numeric_limits<std::int64_t>::max();
    if (numerator > max / ns_per_second) return std::nullopt;
    const auto ns_per_tick_num = numerator * ns_per_second;

    const auto whole = ticks / denominator;
    const auto frac = ticks % denominator;
    if (whole > max / ns_per_tick_num || whole < -max / ns_per_tick_num) {
        return std::nullopt;
    }
    const auto whole_ns = whole * ns_per_tick_num;
    // Split remainder to avoid signed 64-bit overflow in fractional scaling:
    // frac*(N)/D = (frac*(N/D)) + (frac*(N%D))/D.
    const auto integer_ns = ns_per_tick_num / denominator;
    const auto fractional_ns = ns_per_tick_num % denominator;
    // Fast 90kHz / audio-sample / ms time bases, rejecting impossible products.
    const auto absolute_frac = frac >= 0 ? frac : -frac;
    if (integer_ns != 0 && absolute_frac > max / integer_ns) return std::nullopt;
    const auto integer_part = absolute_frac * integer_ns;
    if (fractional_ns != 0 && absolute_frac > max / fractional_ns) return std::nullopt;
    const auto remaining_part = absolute_frac * fractional_ns / denominator;
    if (integer_part > max - remaining_part) return std::nullopt;
    const auto remainder_ns = integer_part + remaining_part;
    if (frac >= 0) {
        if (whole_ns > max - remainder_ns) return std::nullopt;
        return whole_ns + remainder_ns;
    }
    if (whole_ns < std::numeric_limits<std::int64_t>::min() + remainder_ns)
        return std::nullopt;
    return whole_ns - remainder_ns;
}

} // namespace bdmv::core
