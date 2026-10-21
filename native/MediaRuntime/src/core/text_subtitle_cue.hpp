#pragma once
#include "core/media_time.hpp"
#include <string>

namespace lapis::media {

struct TextSubtitleCue final {
    MediaTime start{};
    MediaTime end{};
    std::string text;

    [[nodiscard]] bool valid() const noexcept {
        return end > start && !text.empty();
    }
};

} // namespace lapis::media
