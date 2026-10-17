#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace lapis::media {

enum class MediaLocatorKind : std::uint8_t {
    File,
    Directory,
    Uri,
    OpticalDisc
};

struct MediaLocator final {
    MediaLocatorKind kind{MediaLocatorKind::File};
    std::string value;
};

enum class StreamError : std::uint8_t {
    None,
    InvalidArgument,
    IoFailure,
    Unsupported
};

struct StreamReadResult final {
    std::size_t bytes_read{};
    bool end_of_stream{};
    StreamError error{StreamError::None};

    [[nodiscard]] bool ok() const noexcept { return error == StreamError::None; }
};

enum class SeekOrigin : std::uint8_t {
    Begin,
    Current,
    End
};

struct StreamSeekResult final {
    std::uint64_t position{};
    StreamError error{StreamError::None};

    [[nodiscard]] bool ok() const noexcept { return error == StreamError::None; }
};

class IMediaByteStream {
public:
    virtual ~IMediaByteStream() = default;

    virtual StreamReadResult read(std::span<std::byte> buffer) = 0;
    virtual StreamSeekResult seek(std::int64_t offset, SeekOrigin origin) = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> size() const noexcept = 0;
    [[nodiscard]] virtual bool seekable() const noexcept = 0;
};

enum class SourceError : std::uint8_t {
    None,
    UnsupportedLocator,
    NotFound,
    OpenFailed
};

struct MediaSourceSession final {
    std::unique_ptr<IMediaByteStream> stream;
    std::string demux_hint;
};

struct SourceOpenResult final {
    SourceError error{SourceError::None};
    std::unique_ptr<MediaSourceSession> session;

    [[nodiscard]] bool ok() const noexcept {
        return error == SourceError::None && session != nullptr && session->stream != nullptr;
    }
};

class IMediaSourceProvider {
public:
    virtual ~IMediaSourceProvider() = default;

    [[nodiscard]] virtual bool can_open(const MediaLocator& locator) const noexcept = 0;
    [[nodiscard]] virtual SourceOpenResult open(const MediaLocator& locator) const = 0;
};

} // namespace lapis::media
