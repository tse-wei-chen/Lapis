#pragma once

#include <cstdint>

namespace lapis::media {

struct MediaTime final {
    std::int64_t nanoseconds{};

    auto operator<=>(const MediaTime&) const = default;
};

} // namespace lapis::media
