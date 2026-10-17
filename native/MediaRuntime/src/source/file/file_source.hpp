#pragma once

#include "source/media_source.hpp"

#include <filesystem>
#include <fstream>

namespace lapis::media {

class FileByteStream final : public IMediaByteStream {
public:
    explicit FileByteStream(const std::filesystem::path& path);

    [[nodiscard]] bool is_open() const noexcept;
    StreamReadResult read(std::span<std::byte> buffer) override;
    StreamSeekResult seek(std::int64_t offset, SeekOrigin origin) override;
    [[nodiscard]] std::optional<std::uint64_t> size() const noexcept override;
    [[nodiscard]] bool seekable() const noexcept override;

private:
    std::ifstream stream_;
    std::optional<std::uint64_t> size_;
};

class FileSourceProvider final : public IMediaSourceProvider {
public:
    [[nodiscard]] bool can_open(const MediaLocator& locator) const noexcept override;
    [[nodiscard]] SourceOpenResult open(const MediaLocator& locator) const override;
};

} // namespace lapis::media
