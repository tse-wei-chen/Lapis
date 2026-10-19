#pragma once

#include "navigation/navigation.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace lapis::media::bluray {

// Choose among titles for which the adapter actually retrieved metadata.
// libbluray can report a valid-looking main index even when that title's
// detailed metadata could not be loaded. A missing preferred title must not
// select an unvalidated stream; fall back to the longest enumerated title.
[[nodiscard]] inline std::optional<std::uint32_t> choose_initial_title(
    const NavigationSnapshot& metadata,
    std::optional<std::uint32_t> preferred) noexcept {
    if (metadata.titles.empty()) {
        return std::nullopt;
    }

    if (preferred.has_value()) {
        const auto found = std::find_if(
            metadata.titles.begin(), metadata.titles.end(),
            [preferred](const TitleInfo& title) {
                return title.id == *preferred;
            });
        if (found != metadata.titles.end()) {
            return found->id;
        }
    }

    const auto longest = std::max_element(
        metadata.titles.begin(), metadata.titles.end(),
        [](const TitleInfo& left, const TitleInfo& right) {
            return left.duration.value_or(MediaTime{}) <
                   right.duration.value_or(MediaTime{});
        });
    return longest->id;
}

} // namespace lapis::media::bluray
