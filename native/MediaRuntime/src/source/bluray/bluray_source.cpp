#include "source/bluray/bluray_source.hpp"

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace lapis::media::bluray {
namespace {

std::filesystem::path from_utf8(const std::string& bytes) {
    std::u8string utf8;
    utf8.reserve(bytes.size());
    for (unsigned char c : bytes) {
        utf8.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(utf8);
}

class ByteStream final : public IMediaByteStream {
public:
    explicit ByteStream(std::shared_ptr<IBlurayDisc> disc)
        : disc_(std::move(disc)) {}

    StreamReadResult read(std::span<std::byte> bytes) override {
        return disc_->read(bytes);
    }
    StreamSeekResult seek(std::int64_t offset, SeekOrigin origin) override {
        return disc_->seek(offset, origin);
    }
    [[nodiscard]] std::optional<std::uint64_t> size() const noexcept override {
        return disc_->size();
    }
    [[nodiscard]] bool seekable() const noexcept override { return true; }

private:
    std::shared_ptr<IBlurayDisc> disc_;
};

class Navigation final : public INavigationController {
public:
    explicit Navigation(std::shared_ptr<IBlurayDisc> disc)
        : disc_(std::move(disc)) {}

    [[nodiscard]] NavigationSnapshot snapshot() const override {
        return disc_->navigation();
    }

    [[nodiscard]] NavigationActionResult execute(
        const NavigationCommand& command) override {
        // Do not change libbluray state behind a live FFmpeg demuxer.
        // The actor must eventually stop/advance/reopen the generation.
        const auto current = disc_->navigation();
        if (command.expected_revision.has_value() &&
            *command.expected_revision != current.revision) {
            return {NavigationStatus::StaleSnapshot, false};
        }
        return {NavigationStatus::NotReady, false};
    }

    [[nodiscard]] std::optional<NavigationEvent> poll_event() override {
        return std::nullopt; // Menu and BD-J event bridges are not wired yet.
    }

private:
    std::shared_ptr<IBlurayDisc> disc_;
};

} // namespace

BluraySourceProvider::BluraySourceProvider(DiscOpener opener)
    : opener_(std::move(opener)) {
    if (!opener_) {
        throw std::invalid_argument("Blu-ray source requires a disc backend.");
    }
}

bool BluraySourceProvider::can_open(const MediaLocator& locator) const noexcept {
    return locator.kind == MediaLocatorKind::Directory &&
           !locator.value.empty();
}

SourceOpenResult BluraySourceProvider::open(const MediaLocator& locator) const {
    if (!can_open(locator)) {
        return {SourceError::UnsupportedLocator, nullptr};
    }

    const auto candidate = from_utf8(locator.value);
    std::error_code error;
    if (!std::filesystem::is_directory(candidate, error) || error) {
        return {SourceError::NotFound, nullptr};
    }

    // Accept either the disc root or its BDMV child directory.
    const auto root = candidate.filename() == "BDMV"
        ? candidate.parent_path() : candidate;
    const auto bdmv = root / "BDMV";
    if (!std::filesystem::is_directory(bdmv / "PLAYLIST", error) || error ||
        !std::filesystem::is_directory(bdmv / "STREAM", error) || error ||
        (!std::filesystem::is_regular_file(bdmv / "index.bdmv", error) &&
         !std::filesystem::is_regular_file(bdmv / "index.bdm", error))) {
        return {SourceError::UnsupportedLocator, nullptr};
    }

    // Opening may perform blocking disc I/O. PlaybackMediaSession calls this
    // before the decode generation starts, outside the frame hot path.
    auto opened = opener_(root);
    if (!opened) {
        return {SourceError::OpenFailed, nullptr};
    }

    auto disc = std::shared_ptr<IBlurayDisc>(std::move(opened));
    auto session = std::make_unique<MediaSourceSession>();
    session->navigation = std::make_unique<Navigation>(disc);
    session->stream = std::make_unique<ByteStream>(std::move(disc));
    session->demux_hint = "lapis-bluray.m2ts";
    return {SourceError::None, std::move(session)};
}

} // namespace lapis::media::bluray
