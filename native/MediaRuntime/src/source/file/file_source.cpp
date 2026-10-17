#include "source/file/file_source.hpp"

#include <limits>
#include <system_error>

namespace lapis::media {
namespace {

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const unsigned char character : value) {
        utf8.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(utf8);
}

std::string make_demux_hint(const std::filesystem::path& path) {
    // This is deliberately not the real source path. FFmpeg may use the suffix
    // as a format hint, but all bytes must still arrive through custom AVIO.
    return "lapis-stream" + path.extension().string();
}

} // namespace

FileByteStream::FileByteStream(const std::filesystem::path& path)
    : stream_(path, std::ios::binary) {
    if (!stream_.is_open()) {
        return;
    }

    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (!error) {
        size_ = static_cast<std::uint64_t>(file_size);
    }
}

bool FileByteStream::is_open() const noexcept {
    return stream_.is_open();
}

StreamReadResult FileByteStream::read(std::span<std::byte> buffer) {
    if (!stream_.is_open()) {
        return {0, false, StreamError::IoFailure};
    }

    if (buffer.empty()) {
        return {0, false, StreamError::None};
    }

    if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return {0, false, StreamError::InvalidArgument};
    }

    stream_.read(
        reinterpret_cast<char*>(buffer.data()),
        static_cast<std::streamsize>(buffer.size()));

    const auto count = stream_.gcount();
    if (count < 0) {
        return {0, false, StreamError::IoFailure};
    }

    if (stream_.bad()) {
        return {static_cast<std::size_t>(count), false, StreamError::IoFailure};
    }

    return {
        static_cast<std::size_t>(count),
        stream_.eof(),
        StreamError::None
    };
}

StreamSeekResult FileByteStream::seek(std::int64_t offset, SeekOrigin origin) {
    if (!stream_.is_open()) {
        return {0, StreamError::IoFailure};
    }

    std::ios_base::seekdir direction{};
    switch (origin) {
        case SeekOrigin::Begin:
            direction = std::ios::beg;
            break;
        case SeekOrigin::Current:
            direction = std::ios::cur;
            break;
        case SeekOrigin::End:
            direction = std::ios::end;
            break;
    }

    if (offset < static_cast<std::int64_t>(std::numeric_limits<std::streamoff>::min()) ||
        offset > static_cast<std::int64_t>(std::numeric_limits<std::streamoff>::max())) {
        return {0, StreamError::InvalidArgument};
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), direction);
    if (!stream_) {
        return {0, StreamError::IoFailure};
    }

    const auto position = stream_.tellg();
    if (position < 0) {
        return {0, StreamError::IoFailure};
    }

    return {static_cast<std::uint64_t>(position), StreamError::None};
}

std::optional<std::uint64_t> FileByteStream::size() const noexcept {
    return size_;
}

bool FileByteStream::seekable() const noexcept {
    return true;
}

bool FileSourceProvider::can_open(const MediaLocator& locator) const noexcept {
    return locator.kind == MediaLocatorKind::File && !locator.value.empty();
}

SourceOpenResult FileSourceProvider::open(const MediaLocator& locator) const {
    if (!can_open(locator)) {
        return {SourceError::UnsupportedLocator, nullptr};
    }

    const auto path = path_from_utf8(locator.value);
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return {SourceError::NotFound, nullptr};
    }

    auto stream = std::make_unique<FileByteStream>(path);
    if (!stream->is_open()) {
        return {SourceError::OpenFailed, nullptr};
    }

    auto session = std::make_unique<MediaSourceSession>();
    session->stream = std::move(stream);
    session->demux_hint = make_demux_hint(path);
    return {SourceError::None, std::move(session)};
}

} // namespace lapis::media
