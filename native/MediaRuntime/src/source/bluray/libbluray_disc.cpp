#include "source/bluray/bluray_source.hpp"
#include "source/bluray/title_selection.hpp"

// Only the libbluray adapter sees BLURAY* and the library's structs.
extern "C" {
#include <libbluray/bluray.h>
}

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace lapis::media::bluray {
namespace {

MediaTime ticks_to_media_time(std::uint64_t ticks) noexcept {
    constexpr std::uint64_t TicksPerSecond = 90'000;
    constexpr std::uint64_t NanosecondsPerSecond = 1'000'000'000;
    const auto whole = ticks / TicksPerSecond;
    const auto max_seconds =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
        NanosecondsPerSecond;
    if (whole > max_seconds) {
        return {std::numeric_limits<std::int64_t>::max()};
    }
    const auto nanos = whole * NanosecondsPerSecond +
        (ticks % TicksPerSecond) * NanosecondsPerSecond / TicksPerSecond;
    if (nanos > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return {std::numeric_limits<std::int64_t>::max()};
    }
    return {static_cast<std::int64_t>(nanos)};
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    std::string result;
    result.reserve(bytes.size());
    for (char8_t c : bytes) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

class LibblurayDisc final : public IBlurayDisc {
public:
    LibblurayDisc(std::unique_ptr<BLURAY, decltype(&bd_close)> handle,
                 NavigationSnapshot navigation, std::uint64_t size)
        : handle_(std::move(handle)),
          navigation_(std::move(navigation)),
          size_(size) {}

    [[nodiscard]] NavigationSnapshot navigation() const override {
        // Only discovery metadata is currently published, not live menu
        // state. Playback-generation commands are intentionally unsupported.
        return navigation_;
    }

    StreamReadResult read(std::span<std::byte> bytes) override {
        if (bytes.empty()) return {0, false, StreamError::None};
        std::scoped_lock lock(mutex_);
        const auto length = static_cast<int>(std::min<std::size_t>(
            bytes.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int read_bytes = bd_read(
            handle_.get(), reinterpret_cast<unsigned char*>(bytes.data()), length);
        if (read_bytes < 0) return {0, false, StreamError::IoFailure};
        return {static_cast<std::size_t>(read_bytes),
                read_bytes == 0, StreamError::None};
    }

    StreamSeekResult seek(std::int64_t offset, SeekOrigin origin) override {
        std::scoped_lock lock(mutex_);
        std::uint64_t base{};
        switch (origin) {
            case SeekOrigin::Begin: base = 0; break;
            case SeekOrigin::Current: base = bd_tell(handle_.get()); break;
            case SeekOrigin::End: base = size_; break;
        }

        std::uint64_t target{};
        if (offset < 0) {
            const auto magnitude =
                static_cast<std::uint64_t>(-(offset + 1)) + 1U;
            if (magnitude > base) return {0, StreamError::InvalidArgument};
            target = base - magnitude;
        } else {
            const auto positive = static_cast<std::uint64_t>(offset);
            if (positive > size_ || base > size_ - positive) {
                return {0, StreamError::InvalidArgument};
            }
            target = base + positive;
        }

        if (target > size_ ||
            target > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return {0, StreamError::InvalidArgument};
        }
        const auto resulting_position = bd_seek(handle_.get(), target);
        return resulting_position < 0
            ? StreamSeekResult{0, StreamError::IoFailure}
            : StreamSeekResult{
                static_cast<std::uint64_t>(resulting_position), StreamError::None};
    }

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return size_;
    }

private:
    // Read/seek/tell operate on one libbluray cursor; they never run in
    // parallel even if future sources add concurrent metadata consumers.
    mutable std::mutex mutex_;
    std::unique_ptr<BLURAY, decltype(&bd_close)> handle_;
    const NavigationSnapshot navigation_;
    const std::uint64_t size_;
};

} // namespace

std::unique_ptr<IBlurayDisc> open_libbluray_disc(
    const std::filesystem::path& root) {
    const auto pathname = path_to_utf8(root);
    std::unique_ptr<BLURAY, decltype(&bd_close)> disc(
        bd_open(pathname.c_str(), nullptr), &bd_close);
    if (!disc) return nullptr;

    const auto* disc_info = bd_get_disc_info(disc.get());
    // This first provider deliberately handles only unprotected BDMV. Do
    // not probe key files, load AACS/BD+ helpers or claim DRM support here.
    if (disc_info == nullptr || disc_info->bluray_detected == 0 ||
        disc_info->aacs_detected != 0 || disc_info->bdplus_detected != 0) {
        return nullptr;
    }

    const auto title_count = bd_get_titles(disc.get(), TITLES_RELEVANT, 0);
    // Bound untrusted title enumeration before building metadata.
    if (title_count == 0 || title_count > 4096U) return nullptr;

    NavigationSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.capabilities = NavigationCapability::Titles |
                            NavigationCapability::Playlists;
    snapshot.titles.reserve(title_count);

    for (std::uint32_t index = 0; index < title_count; ++index) {
        std::unique_ptr<BLURAY_TITLE_INFO, decltype(&bd_free_title_info)> info(
            bd_get_title_info(disc.get(), index, 0), &bd_free_title_info);
        if (!info) continue;

        TitleInfo title;
        title.id = index; // libbluray title-list index, not MPLS ID
        title.playlist_id = info->playlist;
        title.duration = ticks_to_media_time(info->duration);
        if (info->chapter_count > 4096U ||
            (info->chapter_count != 0 && info->chapters == nullptr)) {
            return nullptr;
        }
        for (std::uint32_t chapter = 0; chapter < info->chapter_count; ++chapter) {
            title.chapters.push_back({
                info->chapters[chapter].idx,
                ticks_to_media_time(info->chapters[chapter].start)
            });
        }
        for (unsigned angle = 0; angle < info->angle_count; ++angle) {
            title.angles.push_back({angle});
        }
        if (!title.chapters.empty()) {
            snapshot.capabilities =
                snapshot.capabilities | NavigationCapability::Chapters;
        }
        if (title.angles.size() > 1) {
            snapshot.capabilities =
                snapshot.capabilities | NavigationCapability::Angles;
        }
        const auto playlist = info->playlist;
        const auto already_added = std::any_of(
            snapshot.playlists.begin(), snapshot.playlists.end(),
            [playlist](const PlaylistInfo& candidate) {
                return candidate.id == playlist;
            });
        if (!already_added) {
            snapshot.playlists.push_back({playlist, title.duration});
        }
        snapshot.titles.push_back(std::move(title));
    }

    if (snapshot.titles.empty()) return nullptr;
    const int main_title = bd_get_main_title(disc.get());
    const auto preferred = main_title >= 0
        ? std::optional<std::uint32_t>{
            static_cast<std::uint32_t>(main_title)}
        : std::nullopt;
    const auto selected_index = choose_initial_title(snapshot, preferred);
    if (!selected_index.has_value() ||
        bd_select_title(disc.get(), *selected_index) == 0) {
        return nullptr;
    }
    const auto selection = *selected_index;
    const auto bytes = bd_get_title_size(disc.get());
    if (bytes == 0) return nullptr;
    snapshot.selected_title = selection;
    const auto selected = std::find_if(
        snapshot.titles.begin(), snapshot.titles.end(),
        [selection](const TitleInfo& title) { return title.id == selection; });
    if (selected != snapshot.titles.end()) {
        snapshot.selected_playlist = selected->playlist_id;
        if (!selected->chapters.empty()) {
            snapshot.selected_chapter = selected->chapters.front().id;
        }
        if (!selected->angles.empty()) {
            snapshot.selected_angle = selected->angles.front().id;
        }
    }
    // No TopMenu, BD-J or GraphicsOverlay capability is reported.
    return std::make_unique<LibblurayDisc>(
        std::move(disc), std::move(snapshot), bytes);
}

} // namespace lapis::media::bluray
