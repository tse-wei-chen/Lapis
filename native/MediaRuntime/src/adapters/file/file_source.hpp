#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

// File I/O lives in an adapter; no file descriptors or platform filesystem types
// cross the exported C ABI or enter the portable Session domain.
namespace bdmv::adapters {

enum class FileStatus {
    Ok,
    InvalidUri,
    NotRegularFile,
    IoError,
    EndOfFile,
    OffsetOutOfRange
};

struct FileReadResult {
    FileStatus status{FileStatus::IoError};
    std::size_t bytes_read{0};
};

class FileSource final {
public:
    static FileStatus open(std::string_view utf8_uri,
                           std::unique_ptr<FileSource>& out);

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] FileReadResult read_at(std::uint64_t offset,
                                         std::span<std::byte> destination) noexcept;

private:
    FileSource(std::ifstream&& stream, std::uint64_t size)
        : stream_(std::move(stream)), size_(size) {}

    std::ifstream stream_;
    std::uint64_t size_{0};
    std::mutex mutex_;  // read_at is safe for simultaneous workers and seeks.
};

} // namespace bdmv::adapters
