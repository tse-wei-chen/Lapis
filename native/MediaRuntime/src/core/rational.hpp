#pragma once

#include <compare>
#include <cstdint>

namespace lapis::media {

struct Rational final {
    std::int32_t numerator{};
    std::int32_t denominator{1};

    auto operator<=>(const Rational&) const = default;
};

struct SourceTimestamp final {
    std::int64_t value{};
    Rational time_base{};
};

} // namespace lapis::media
