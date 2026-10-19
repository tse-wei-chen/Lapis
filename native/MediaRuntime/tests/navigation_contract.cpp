#include "navigation/navigation.hpp"
#include "navigation/chapter_seek.hpp"
#include "session/playback_media_session.hpp"
#include "source/file/file_source.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {
struct Lifetime final {
    bool navigation_alive{};
    bool stream_closed_first{};
    bool navigation_closed{};
};
class FakeNavigation final : public lapis::media::INavigationController {
public:
    explicit FakeNavigation(std::shared_ptr<Lifetime> life) : life_(std::move(life)) {
        life_->navigation_alive = true;
    }
    ~FakeNavigation() override {
        life_->navigation_alive = false;
        life_->navigation_closed = true;
    }
    [[nodiscard]] lapis::media::NavigationSnapshot snapshot() const override {
        using namespace lapis::media;
        NavigationSnapshot out;
        out.revision = revision_;
        out.capabilities = NavigationCapability::Titles |
            NavigationCapability::Chapters | NavigationCapability::TopMenu;
        TitleInfo title;
        title.id = 7;
        title.playlist_id = 12;
        title.chapters.push_back({4, MediaTime{0}});
        title.angles.push_back({1});
        out.titles.push_back(std::move(title));
        out.playlists.push_back({12, MediaTime{2'000'000'000}});
        out.selected_title = 7;
        out.selected_playlist = 12;
        return out;
    }
    [[nodiscard]] lapis::media::NavigationActionResult execute(
        const lapis::media::NavigationCommand& cmd) override {
        using namespace lapis::media;
        if (cmd.expected_revision && *cmd.expected_revision != revision_) {
            return {NavigationStatus::StaleSnapshot, false};
        }
        if (cmd.kind != NavigationCommandKind::SelectTitle || cmd.id != 7) {
            return {NavigationStatus::Unsupported, false};
        }
        ++revision_;
        return {NavigationStatus::Applied, true};
    }
    [[nodiscard]] std::optional<lapis::media::NavigationEvent> poll_event() override {
        if (event_read_) return std::nullopt;
        event_read_ = true;
        return lapis::media::NavigationEvent{
            lapis::media::NavigationEventKind::TitleChanged, revision_, 7, nullptr};
    }
private:
    std::shared_ptr<Lifetime> life_;
    std::uint64_t revision_{1};
    bool event_read_{};
};
class BorrowedStream final : public lapis::media::IMediaByteStream {
public:
    BorrowedStream(std::unique_ptr<lapis::media::IMediaByteStream> source,
                   std::shared_ptr<Lifetime> life)
        : source_(std::move(source)), life_(std::move(life)) {}
    ~BorrowedStream() override {
        life_->stream_closed_first = life_->navigation_alive;
    }
    lapis::media::StreamReadResult read(std::span<std::byte> bytes) override {
        return source_->read(bytes);
    }
    lapis::media::StreamSeekResult seek(
        std::int64_t offset, lapis::media::SeekOrigin origin) override {
        return source_->seek(offset, origin);
    }
    [[nodiscard]] std::optional<std::uint64_t> size() const noexcept override {
        return source_->size();
    }
    [[nodiscard]] bool seekable() const noexcept override {
        return source_->seekable();
    }
private:
    std::unique_ptr<lapis::media::IMediaByteStream> source_;
    std::shared_ptr<Lifetime> life_;
};
class FakeDiscProvider final : public lapis::media::IMediaSourceProvider {
public:
    explicit FakeDiscProvider(std::shared_ptr<Lifetime> life)
        : life_(std::move(life)) {}
    [[nodiscard]] bool can_open(
        const lapis::media::MediaLocator& locator) const noexcept override {
        return locator.kind == lapis::media::MediaLocatorKind::OpticalDisc;
    }
    [[nodiscard]] lapis::media::SourceOpenResult open(
        const lapis::media::MediaLocator& locator) const override {
        using namespace lapis::media;
        if (!can_open(locator)) return {SourceError::UnsupportedLocator, nullptr};
        auto file = std::make_unique<FileByteStream>(LAPIS_TEST_MEDIA_PATH);
        if (!file->is_open()) return {SourceError::OpenFailed, nullptr};
        auto source = std::make_unique<MediaSourceSession>();
        source->navigation = std::make_unique<FakeNavigation>(life_);
        source->stream = std::make_unique<BorrowedStream>(std::move(file), life_);
        source->demux_hint = "lapis-virtual.y4m";
        return {SourceError::None, std::move(source)};
    }
private:
    std::shared_ptr<Lifetime> life_;
};
} // namespace

int main() {
    using namespace lapis::media;
    CHECK(supports_navigation(NavigationCapability::Titles |
                              NavigationCapability::TopMenu,
                              NavigationCapability::TopMenu));
    CHECK(!supports_navigation(NavigationCapability::Titles,
                               NavigationCapability::PopupMenu));

    NavigationOverlay image;
    image.width = 2;
    image.height = 2;
    image.stride_bytes = 8;
    image.premultiplied_bgra.resize(16);
    CHECK(image.has_valid_pixels());
    image.stride_bytes = 7;
    CHECK(!image.has_valid_pixels());
    image.stride_bytes = 8;
    image.premultiplied_bgra.pop_back();
    CHECK(!image.has_valid_pixels());

    auto life = std::make_shared<Lifetime>();
    {
        PlaybackMediaSession media;
        CHECK(!media.navigation_snapshot());
        media.register_source_provider(std::make_unique<FakeDiscProvider>(life));
        const auto opened = media.open(
            {MediaLocatorKind::OpticalDisc, "synthetic-clear-disc"});
        CHECK(opened.ok());
        CHECK(opened.has_video);
        const auto info = media.navigation_snapshot();
        CHECK(info.has_value());
        CHECK(info->selected_title == std::uint32_t{7});
        CHECK(info->titles.size() == 1);
        CHECK(info->titles[0].chapters[0].id == 4);
        CHECK(selected_chapter_start(*info, 4) == MediaTime{0});
        CHECK(!selected_chapter_start(*info, 5).has_value());
        auto second_title = *info;
        TitleInfo alternate;
        alternate.id = 8;
        alternate.chapters.push_back({4, MediaTime{99'000'000'000}});
        second_title.titles.push_back(alternate);
        CHECK(selected_chapter_start(second_title, 4) == MediaTime{0});
        second_title.selected_title = 8;
        CHECK(selected_chapter_start(second_title, 4) ==
              MediaTime{99'000'000'000});
        auto other_title = *info;
        other_title.selected_title = 1234;
        CHECK(!selected_chapter_start(other_title, 4).has_value());
        other_title = *info;
        other_title.capabilities = NavigationCapability::Titles;
        CHECK(!selected_chapter_start(other_title, 4).has_value());
        other_title = *info;
        other_title.titles[0].chapters[0].start = MediaTime{-1};
        CHECK(!selected_chapter_start(other_title, 4).has_value());
        CHECK(supports_navigation(info->capabilities, NavigationCapability::TopMenu));
        CHECK(!supports_navigation(info->capabilities, NavigationCapability::BdJ));

        bool rejected = false;
        try {
            media.register_source_provider(std::make_unique<FakeDiscProvider>(life));
        } catch (const std::logic_error&) {
            rejected = true;
        }
        CHECK(rejected);
        const auto file_opened = media.open(
            {MediaLocatorKind::File, LAPIS_TEST_MEDIA_PATH});
        CHECK(file_opened.ok());
        CHECK(!media.navigation_snapshot());
        CHECK(life->stream_closed_first);
        CHECK(life->navigation_closed);
        CHECK(info->titles[0].id == 7);
    }

    auto other_life = std::make_shared<Lifetime>();
    FakeNavigation fake(other_life);
    const auto stale = fake.execute({
        NavigationCommandKind::SelectTitle, 7, 0, 0, 0});
    CHECK(stale.status == NavigationStatus::StaleSnapshot);
    CHECK(!stale.stream_discontinuity);
    const auto changed = fake.execute({
        NavigationCommandKind::SelectTitle, 7, 0, 0, 1});
    CHECK(changed.ok());
    CHECK(changed.stream_discontinuity);
    CHECK(fake.snapshot().revision == 2);
    const auto event = fake.poll_event();
    CHECK(event.has_value());
    CHECK(event->kind == NavigationEventKind::TitleChanged);
    CHECK(event->revision == 2);
    CHECK(!fake.poll_event());
    return 0;
}
