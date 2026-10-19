#include "session/playback_media_session.hpp"
#include "source/bluray/bluray_source.hpp"
#include "source/bluray/title_selection.hpp"
#include "source/file/file_source.hpp"
#include "source/media_source_registry.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {
struct Probe {
    int opens{};
    int closes{};
    int reads{};
    int seeks{};
};

std::string utf8_path(const std::filesystem::path& path) {
    auto bytes = path.u8string();
    std::string result;
    for (char8_t c : bytes) result.push_back(static_cast<char>(c));
    return result;
}

struct TemporaryDisc final {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("lapis-bdmv-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    TemporaryDisc() {
        std::filesystem::create_directories(root / "BDMV" / "PLAYLIST");
        std::filesystem::create_directories(root / "BDMV" / "STREAM");
        std::ofstream(root / "BDMV" / "index.bdmv").put('B');
    }
    ~TemporaryDisc() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

class FailingDirectoryProvider final : public lapis::media::IMediaSourceProvider {
public:
    explicit FailingDirectoryProvider(int& called) : called_(called) {}
    [[nodiscard]] bool can_open(
        const lapis::media::MediaLocator& locator) const noexcept override {
        return locator.kind == lapis::media::MediaLocatorKind::Directory;
    }
    [[nodiscard]] lapis::media::SourceOpenResult open(
        const lapis::media::MediaLocator&) const override {
        ++called_;
        return {lapis::media::SourceError::OpenFailed, nullptr};
    }

private:
    int& called_;
};

class FakeDisc final : public lapis::media::bluray::IBlurayDisc {
public:
    explicit FakeDisc(std::shared_ptr<Probe> probe)
        : probe_(std::move(probe)), stream_(LAPIS_TEST_MEDIA_PATH) {
        ++probe_->opens;
    }
    ~FakeDisc() override { ++probe_->closes; }

    [[nodiscard]] lapis::media::NavigationSnapshot navigation() const override {
        using namespace lapis::media;
        NavigationSnapshot snap;
        snap.revision = 1;
        snap.capabilities = NavigationCapability::Titles |
                            NavigationCapability::Playlists |
                            NavigationCapability::Chapters;
        TitleInfo title;
        title.id = 2;
        title.playlist_id = 105;
        title.chapters.push_back({0, MediaTime{0}});
        snap.titles.push_back(std::move(title));
        snap.playlists.push_back({105, MediaTime{2'000'000'000}});
        snap.selected_title = 2;
        snap.selected_playlist = 105;
        snap.selected_chapter = 0;
        return snap;
    }

    lapis::media::StreamReadResult read(std::span<std::byte> bytes) override {
        ++probe_->reads;
        return stream_.read(bytes);
    }
    lapis::media::StreamSeekResult seek(
        std::int64_t offset, lapis::media::SeekOrigin origin) override {
        ++probe_->seeks;
        return stream_.seek(offset, origin);
    }
    [[nodiscard]] std::uint64_t size() const noexcept override {
        return stream_.size().value_or(0);
    }

private:
    std::shared_ptr<Probe> probe_;
    lapis::media::FileByteStream stream_;
};
} // namespace

int main() {
    using namespace lapis::media;
    using lapis::media::bluray::BluraySourceProvider;

    // Main-title metadata sometimes fails to load. Selection must only
    // return an index which is actually present in our discovery snapshot.
    NavigationSnapshot candidates;
    CHECK(!bluray::choose_initial_title(candidates, std::uint32_t{10}));
    TitleInfo short_title;
    short_title.id = 4;
    short_title.duration = MediaTime{3'000'000'000};
    TitleInfo long_title;
    long_title.id = 9;
    long_title.duration = MediaTime{7'000'000'000};
    candidates.titles.push_back(short_title);
    candidates.titles.push_back(long_title);
    CHECK(bluray::choose_initial_title(candidates, std::uint32_t{4}) ==
          std::optional<std::uint32_t>{4});
    CHECK(bluray::choose_initial_title(candidates, std::uint32_t{100}) ==
          std::optional<std::uint32_t>{9});
    CHECK(bluray::choose_initial_title(candidates, std::nullopt) ==
          std::optional<std::uint32_t>{9});
    candidates.titles.front().duration = long_title.duration;
    CHECK(bluray::choose_initial_title(candidates, std::nullopt) ==
          std::optional<std::uint32_t>{4});

    TemporaryDisc disc;
    const auto name = utf8_path(disc.root);
    auto probe = std::make_shared<Probe>();
    int attempted = 0;
    BluraySourceProvider provider([probe, &attempted](
        const std::filesystem::path& root)
        -> std::unique_ptr<bluray::IBlurayDisc> {
        ++attempted;
        if (!std::filesystem::exists(root / "BDMV" / "index.bdmv")) {
            return nullptr;
        }
        return std::make_unique<FakeDisc>(probe);
    });

    CHECK(provider.can_open({MediaLocatorKind::Directory, name}));
    CHECK(!provider.can_open({MediaLocatorKind::OpticalDisc, name}));
    CHECK(!provider.can_open({MediaLocatorKind::File, name}));
    CHECK(provider.open({MediaLocatorKind::File, name}).error ==
          SourceError::UnsupportedLocator);
    CHECK(provider.open({MediaLocatorKind::Directory, name + "-absent"}).error ==
          SourceError::NotFound);
    CHECK(attempted == 0);

    // Explicitly installed providers must win over built-in libbluray
    // even when LAPIS_ENABLE_LIBBLURAY is ON. Failure must not fallback.
    int blocked_opens = 0;
    {
        PlaybackMediaSession overridden;
        overridden.register_source_provider(
            std::make_unique<BluraySourceProvider>(
                [probe](const std::filesystem::path&) {
                    return std::make_unique<FakeDisc>(probe);
                }));
        overridden.register_source_provider(
            std::make_unique<FailingDirectoryProvider>(blocked_opens));
        CHECK(overridden.open({MediaLocatorKind::Directory, name}).error ==
              MediaOpenError::SourceOpenFailed);
        CHECK(blocked_opens == 1);
        CHECK(probe->opens == 0);
    }

    const auto bdmv_path = utf8_path(disc.root / "BDMV");
    {
        auto direct = provider.open({MediaLocatorKind::Directory, bdmv_path});
        CHECK(direct.ok());
        CHECK(direct.session->demux_hint == "lapis-bluray.m2ts");
        CHECK(direct.session->navigation != nullptr);
        CHECK(direct.session->stream->seekable());
        CHECK(direct.session->stream->size().value_or(0) > 0);
        CHECK(direct.session->stream->seek(0, SeekOrigin::Begin).ok());
        auto snap = direct.session->navigation->snapshot();
        CHECK(snap.selected_title == std::uint32_t{2});
        CHECK(snap.selected_playlist == std::uint32_t{105});
        CHECK(snap.titles.size() == 1);
        CHECK(supports_navigation(snap.capabilities, NavigationCapability::Chapters));
        CHECK(!supports_navigation(snap.capabilities, NavigationCapability::BdJ));
        CHECK(!supports_navigation(snap.capabilities, NavigationCapability::TopMenu));
        CHECK(direct.session->navigation->execute({
            NavigationCommandKind::SelectTitle, 2, 0, 0, 0}).status ==
            NavigationStatus::StaleSnapshot);
        CHECK(direct.session->navigation->execute({
            NavigationCommandKind::SelectTitle, 2, 0, 0, 1}).status ==
            NavigationStatus::NotReady);
        CHECK(!direct.session->navigation->poll_event().has_value());
    }
    CHECK(probe->opens == 1);
    CHECK(probe->closes == 1);

    {
        PlaybackMediaSession media;
        media.register_source_provider(
            std::make_unique<BluraySourceProvider>(
                [probe](const std::filesystem::path&) {
                    return std::make_unique<FakeDisc>(probe);
                }));
        const auto opened = media.open({MediaLocatorKind::Directory, name});
        CHECK(opened.ok());
        CHECK(opened.has_video); // The unmodified FFmpeg AVIO still decodes.
        CHECK(!opened.has_audio);
        const auto snap = media.navigation_snapshot();
        CHECK(snap.has_value());
        CHECK(snap->selected_playlist == std::uint32_t{105});
        CHECK(probe->reads > 0);
        CHECK(probe->seeks > 0);

        const auto replaced = media.open({
            MediaLocatorKind::File, LAPIS_TEST_MEDIA_PATH});
        CHECK(replaced.ok());
        CHECK(!media.navigation_snapshot().has_value());
        CHECK(probe->closes == 2);
        CHECK(snap->titles[0].id == std::uint32_t{2}); // Own metadata survives replacement.
    }
    CHECK(probe->opens == 2);
    CHECK(probe->closes == 2);

    // Registry composition must reject null overrides as early as built-ins.
    MediaSourceRegistry registry;
    bool rejected_null_override = false;
    try {
        registry.add_override(nullptr);
    } catch (const std::invalid_argument&) {
        rejected_null_override = true;
    }
    CHECK(rejected_null_override);
    return 0;
}
