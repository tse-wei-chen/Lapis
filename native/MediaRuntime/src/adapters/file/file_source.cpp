#include "adapters/file/file_source.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

namespace bdmv::adapters {
namespace {

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Local file: URIs and plain OS paths only. No network/UNC hosts are accepted.
FileStatus parse_local_path(std::string_view uri, std::filesystem::path& path) {
    if (uri.empty() || uri.find('\0') != std::string_view::npos) return FileStatus::InvalidUri;
    bool file_uri = false;
    if (uri.starts_with("file://")) {
        file_uri = true;
        uri.remove_prefix(7);
        if (uri.starts_with("localhost/")) uri.remove_prefix(9);
        else if (uri.empty() || uri.front() != '/') return FileStatus::InvalidUri;
    } else if (uri.find("://") != std::string_view::npos) {
        return FileStatus::InvalidUri;
    }

    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        const char c = uri[i];
        if (file_uri && (c == '?' || c == '#')) return FileStatus::InvalidUri;
        if (file_uri && c == '%') {
            if (i + 2 >= uri.size()) return FileStatus::InvalidUri;
            const int hi = hex_digit(uri[i + 1]), lo = hex_digit(uri[i + 2]);
            if (hi < 0 || lo < 0) return FileStatus::InvalidUri;
            const char value = static_cast<char>((hi << 4) | lo);
            if (value == '\0') return FileStatus::InvalidUri;
            decoded.push_back(value);
            i += 2;
        } else {
            decoded.push_back(c);
        }
    }
#ifdef _WIN32
    // RFC 8089 file:///C:/path maps to C:\path on Windows.
    if (file_uri && decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':') {
        decoded.erase(0, 1);
    }
#endif
    if (decoded.empty()) return FileStatus::InvalidUri;
    path = std::filesystem::path(std::u8string(decoded.begin(), decoded.end()));
    return FileStatus::Ok;
}

} // namespace

FileStatus FileSource::open(std::string_view uri, std::unique_ptr<FileSource>& out) {
    out.reset();
    try {
        std::filesystem::path path;
        const auto status = parse_local_path(uri, path);
        if (status != FileStatus::Ok) return status;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return (!error || error == std::errc::no_such_file_or_directory)
                ? FileStatus::NotRegularFile : FileStatus::IoError;
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error) return FileStatus::IoError;
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) return FileStatus::IoError;
        out.reset(new FileSource(std::move(stream), size));
        return FileStatus::Ok;
    } catch (const std::bad_alloc&) {
        return FileStatus::IoError;
    } catch (const std::filesystem::filesystem_error&) {
        return FileStatus::IoError;
    } catch (...) {
        return FileStatus::InvalidUri;
    }
}

FileReadResult FileSource::read_at(std::uint64_t offset,
                                   std::span<std::byte> destination) noexcept {
    std::lock_guard guard(mutex_);
    if (offset > size_) return {FileStatus::OffsetOutOfRange, 0};
    if (destination.empty()) return {FileStatus::Ok, 0};
    if (offset == size_) return {FileStatus::EndOfFile, 0};
    constexpr auto max_offset = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max());
    if (offset > max_offset) return {FileStatus::OffsetOutOfRange, 0};
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        std::min<std::uint64_t>(size_ - offset, destination.size()),
        static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())));
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return {FileStatus::IoError, 0};
    stream_.read(reinterpret_cast<char*>(destination.data()),
                 static_cast<std::streamsize>(amount));
    const auto actual = static_cast<std::size_t>(stream_.gcount());
    if (stream_.bad() || actual != amount) return {FileStatus::IoError, actual};
    return {FileStatus::Ok, actual};
}

} // namespace bdmv::adapters
