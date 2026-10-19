#pragma once

#include "navigation/navigation.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace lapis::media {

// Pure, owned-metadata lookup. A chapter belongs only to the selected title;
// never interpret a chapter ID as an array index or cross into other titles.
// Do not execute libbluray navigation commands here: the caller's playback
// actor must perform its normal epoch-advancing seek after this read.
[[nodiscard]] inline std::optional<MediaTime> selected_chapter_start(
    const NavigationSnapshot& state, std::uint32_t chapter_id) noexcept {
    if (!supports_navigation(state.capabilities, NavigationCapability::Chapters) ||
        !state.selected_title.has_value()) {
        return std::nullopt;
    }
    const auto title = std::find_if(state.titles.begin(), state.titles.end(),
        [&](const TitleInfo& t) { return t.id == *state.selected_title; });
    if (title == state.titles.end()) {
        return std::nullopt;
    }
    const auto chapter = std::find_if(title->chapters.begin(), title->chapters.end(),
        [&](const ChapterInfo& c) { return c.id == chapter_id; });
    if (chapter == title->chapters.end() ||
        chapter->start.nanoseconds < 0) {
        return std::nullopt;
    }
    return chapter->start;
}

} // namespace lapis::media
