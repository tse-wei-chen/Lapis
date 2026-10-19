#pragma once

#include "core/media_time.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lapis::media {

// Core-owned names, independent from libbluray, BD-J and GPU APIs.
// Navigation is NOT a content-protection/decryption capability.
enum class NavigationCapability : std::uint32_t {
    None = 0,
    Titles = 1U << 0,
    Playlists = 1U << 1,
    Chapters = 1U << 2,
    Angles = 1U << 3,
    TopMenu = 1U << 4,
    PopupMenu = 1U << 5,
    MenuInput = 1U << 6,
    GraphicsOverlay = 1U << 7,
    BdJ = 1U << 8
};

[[nodiscard]] constexpr NavigationCapability operator|(
    NavigationCapability a, NavigationCapability b) noexcept {
    return static_cast<NavigationCapability>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool supports_navigation(
    NavigationCapability available, NavigationCapability requested) noexcept {
    return (static_cast<std::uint32_t>(available) &
            static_cast<std::uint32_t>(requested)) ==
           static_cast<std::uint32_t>(requested);
}

struct ChapterInfo final {
    std::uint32_t id{};
    MediaTime start{};
};
struct AngleInfo final { std::uint32_t id{}; };
struct PlaylistInfo final {
    std::uint32_t id{};
    std::optional<MediaTime> duration;
};
struct TitleInfo final {
    std::uint32_t id{};
    std::optional<std::uint32_t> playlist_id;
    std::optional<MediaTime> duration;
    std::vector<ChapterInfo> chapters;
    std::vector<AngleInfo> angles;
};

// Owned metadata, never a borrowed BLURAY_TITLE_INFO or source pointer.
// A navigation revision is local to this source, NOT a PlaybackEpoch.
struct NavigationSnapshot final {
    std::uint64_t revision{};
    NavigationCapability capabilities{NavigationCapability::None};
    std::vector<TitleInfo> titles;
    std::vector<PlaylistInfo> playlists;
    std::optional<std::uint32_t> selected_title;
    std::optional<std::uint32_t> selected_playlist;
    std::optional<std::uint32_t> selected_chapter;
    std::optional<std::uint32_t> selected_angle;
    bool menu_visible{};
};

enum class NavigationCommandKind : std::uint8_t {
    FirstPlay,
    SelectTitle,
    SelectPlaylist,
    SelectChapter,
    SelectAngle,
    ShowTopMenu,
    ShowPopupMenu,
    MenuUp, MenuDown, MenuLeft, MenuRight, MenuActivate, MenuBack,
    PointerMove, PointerActivate
};

struct NavigationCommand final {
    NavigationCommandKind kind{NavigationCommandKind::FirstPlay};
    std::uint32_t id{}; // Stable source-defined ID, not a vector index.
    std::int32_t x{}; // Menu-space pointer position.
    std::int32_t y{};
    std::optional<std::uint64_t> expected_revision;
};

enum class NavigationStatus : std::uint8_t {
    Applied, Unsupported, InvalidSelection, StaleSnapshot, NotReady, Failed
};

struct NavigationActionResult final {
    NavigationStatus status{NavigationStatus::Unsupported};
    // An owning Playback actor MUST retire the old decode generation and
    // advance PlaybackEpoch before using a new virtual MPEG-TS position.
    bool stream_discontinuity{};
    [[nodiscard]] bool ok() const noexcept {
        return status == NavigationStatus::Applied;
    }
};

// Menu graphics are not subtitle frames. The source adapter copies BD-J
// callback-owned pixels to this immutable-by-sharing BGRA8 premultiplied
// payload; the future compositor consumes it independently of subtitles.
struct NavigationOverlay final {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride_bytes{};
    std::vector<std::byte> premultiplied_bgra;

    [[nodiscard]] bool has_valid_pixels() const noexcept {
        return width != 0 && height != 0 &&
            static_cast<std::uint64_t>(stride_bytes) >=
                static_cast<std::uint64_t>(width) * 4U &&
            static_cast<std::uint64_t>(stride_bytes) * height <=
                premultiplied_bgra.size();
    }
};

enum class NavigationEventKind : std::uint8_t {
    TitleChanged, PlaylistChanged, ChapterChanged, AngleChanged,
    MenuShown, MenuHidden, OverlayUpdated, OverlayCleared,
    StreamDiscontinuity, Faulted
};
struct NavigationEvent final {
    NavigationEventKind kind{NavigationEventKind::Faulted};
    std::uint64_t revision{};
    std::optional<std::uint32_t> id;
    std::shared_ptr<const NavigationOverlay> overlay;
};

// The orchestrator must serialize execute() with demux/seek state; this
// interface alone grants no right to mutate a running playback generation.
// A future libbluray/JVM callback adapter copies events to a bounded queue;
// poll_event() must not block and never returns foreign-owned memory.
class INavigationController {
public:
    virtual ~INavigationController() = default;
    [[nodiscard]] virtual NavigationSnapshot snapshot() const = 0;
    [[nodiscard]] virtual NavigationActionResult execute(
        const NavigationCommand& command) = 0;
    [[nodiscard]] virtual std::optional<NavigationEvent> poll_event() = 0;
};

} // namespace lapis::media
