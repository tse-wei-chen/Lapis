#include "adapters/file/file_source.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>

// Developer-only file I/O probe; NOT a demuxer nor a media playback CLI.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: bdmv_file_probe <local-file-or-file-uri>\n";
        return 2;
    }
    std::unique_ptr<bdmv::adapters::FileSource> file;
    const auto status = bdmv::adapters::FileSource::open(argv[1], file);
    if (status != bdmv::adapters::FileStatus::Ok) {
        std::cerr << "FileSource open failed: " << static_cast<int>(status) << '\n';
        return 1;
    }
    std::array<std::byte, 16> first{};
    const auto first_result = file->read_at(0, first);
    if (first_result.status != bdmv::adapters::FileStatus::Ok &&
        first_result.status != bdmv::adapters::FileStatus::EndOfFile) return 1;
    std::array<std::byte, 16> last{};
    const auto last_result = file->read_at(file->size() > last.size() ?
                                           file->size() - last.size() : 0, last);
    if (last_result.status != bdmv::adapters::FileStatus::Ok &&
        last_result.status != bdmv::adapters::FileStatus::EndOfFile) return 1;
    std::cout << "{\"file_size\":" << file->size() << ",\"prefix_hex\":\"";
    for (std::size_t i = 0; i < first_result.bytes_read; ++i)
        std::cout << std::hex << std::setfill('0') << std::setw(2)
                  << std::to_integer<unsigned>(first[i]);
    std::cout << "\",\"tail_bytes\":" << std::dec << last_result.bytes_read << "}\n";
}
