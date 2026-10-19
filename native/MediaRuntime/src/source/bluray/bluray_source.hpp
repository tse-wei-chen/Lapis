#pragma once

#include "source/media_source.hpp"

#include <filesystem>
#include <functional>
#include <memory>

namespace lapis::media::bluray {

// A logical disc is owned by this adapter, never by FFmpeg or Application.
// The system implementation wraps BLURAY*; tests supply an in-memory backend.
class IBlurayDisc {
public:
    virtual ~IBlurayDisc() = default;
    [[nodiscard]] virtual NavigationSnapshot navigation() const = 0;
    virtual StreamReadResult read(std::span<std::byte> bytes) = 0;
    virtual StreamSeekResult seek(std::int64_t offset, SeekOrigin origin) = 0;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
};

using DiscOpener = std::function<std::unique_ptr<IBlurayDisc>(
    const std::filesystem::path& root)>;

// This first slice supports BDMV directories only, not drive devices/ISOs.
// Navigation is discovery-only until actor-coordinated commands exist.
class BluraySourceProvider final : public IMediaSourceProvider {
public:
    explicit BluraySourceProvider(DiscOpener opener);
    [[nodiscard]] bool can_open(const MediaLocator& locator) const noexcept override;
    [[nodiscard]] SourceOpenResult open(const MediaLocator& locator) const override;

private:
    DiscOpener opener_;
};

// Linked only when LAPIS_ENABLE_LIBBLURAY=ON. The implementation rejects
// AACS/BD+ discs; DRM integration belongs to a different future provider.
[[nodiscard]] std::unique_ptr<IBlurayDisc> open_libbluray_disc(
    const std::filesystem::path& root);

} // namespace lapis::media::bluray
