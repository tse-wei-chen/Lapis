#include "adapters/file/file_source.hpp"
#include "core/epoch_queue.hpp"
#include "core/media_time.hpp"
#include "core/playback_clock.hpp"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace bdmv;

void test_clock() {
    core::PlaybackClock clock;
    clock.reanchor(2'000'000'000LL, 100);
    assert(clock.position(600) == 2'000'000'000LL);
    clock.play(1'000'000'000LL);
    assert(clock.position(1'250'000'000LL) == 2'250'000'000LL);
    clock.pause(1'500'000'000LL);
    assert(clock.position(9'000'000'000LL) == 2'500'000'000LL);
    clock.play(10'000'000'000LL);
    assert(clock.position(10'100'000'000LL) == 2'600'000'000LL);
    clock.seek(700'000'000LL, 10'200'000'000LL);
    assert(clock.position(10'500'000'000LL) == 1'000'000'000LL);
    assert(clock.position(10'000'000'000LL) == 700'000'000LL); // no backwards motion
    clock.seek(std::numeric_limits<std::int64_t>::max() - 3, 0);
    assert(clock.position(100) == std::numeric_limits<std::int64_t>::max());
}

void test_timebase() {
    auto ns = core::timebase_to_ns(90000, 1, 90000);
    assert(ns.has_value() && *ns == 1'000'000'000LL);
    assert(core::timebase_to_ns(45000, 1, 90000) == 500'000'000LL);
    assert(core::timebase_to_ns(-45000, 1, 90000) == -500'000'000LL);
    assert(core::timebase_to_ns(48000, 1, 48000) == 1'000'000'000LL);
    assert(core::timebase_to_ns(90000, 1, 0) == std::nullopt);
    assert(core::timebase_to_ns(std::numeric_limits<std::int64_t>::max(), 1, 1) == std::nullopt);
}

void test_epoch_queue() {
    core::EpochQueue<int> queue(2);
    queue.reset(42, 5);
    assert(queue.try_push({42, 5, 1000, 1}));
    assert(!queue.try_push({42, 4, 900, 2}));
    assert(!queue.try_push({43, 5, 900, 2}));
    assert(queue.try_push({42, 5, 2000, 3}));
    assert(!queue.try_push({42, 5, 3000, 4}));
    auto first = queue.try_pop();
    assert(first && first->value == 1 && first->pts_ns == 1000);
    queue.reset(42, 6);
    assert(queue.size() == 0 && !queue.try_push({42, 5, 2000, 3}));
    assert(queue.try_push({42, 6, 999, 6}));
    queue.close();
    assert(!queue.try_pop().has_value());
    assert(!queue.try_push({42, 6, 1000, 7}));

    core::EpochQueue<int> drop_old(1, core::OverflowPolicy::DropOldest);
    drop_old.reset(1, 2);
    assert(drop_old.try_push({1, 2, 1, 1}));
    assert(drop_old.try_push({1, 2, 2, 2}));
    assert(drop_old.try_pop()->value == 2);
}

void test_file() {
    const auto root = std::filesystem::temp_directory_path() / "bdmv-source-tests";
    std::filesystem::create_directories(root);
    const auto file = root / "test source.bin";
    {
        std::ofstream write(file, std::ios::binary | std::ios::trunc);
        for (unsigned i = 0; i < 4096; ++i) write.put(static_cast<char>(i & 0xff));
        assert(write.good());
    }
    std::unique_ptr<adapters::FileSource> source;
    assert(adapters::FileSource::open(file.u8string().empty() ? "" : file.string(), source)
           == adapters::FileStatus::Ok);
    assert(source && source->size() == 4096);
    std::array<std::byte, 80> buffer{};
    const auto part = source->read_at(1000, buffer);
    assert(part.status == adapters::FileStatus::Ok && part.bytes_read == 80);
    for (unsigned i = 0; i < 80; ++i) {
        assert(std::to_integer<unsigned>(buffer[i]) == ((1000 + i) & 0xff));
    }
    auto tail = source->read_at(4090, buffer);
    assert(tail.status == adapters::FileStatus::Ok && tail.bytes_read == 6);
    assert(source->read_at(4096, buffer).status == adapters::FileStatus::EndOfFile);
    assert(source->read_at(4097, buffer).status == adapters::FileStatus::OffsetOutOfRange);
    assert(source->read_at(4096, std::span<std::byte>{}).status == adapters::FileStatus::Ok);

    std::string uri = "file://" + file.generic_string();
    const auto pos = uri.find("test source.bin");
    assert(pos != std::string::npos);
    uri.replace(pos, 15, "test%20source.bin");
    std::unique_ptr<adapters::FileSource> encoded_source;
    assert(adapters::FileSource::open(uri, encoded_source) == adapters::FileStatus::Ok);
    assert(encoded_source->size() == 4096);
    assert(adapters::FileSource::open("http://example.com/a.mkv", encoded_source)
           == adapters::FileStatus::InvalidUri);
    assert(!encoded_source);
    assert(adapters::FileSource::open("file://remotehost/a.mkv", encoded_source)
           == adapters::FileStatus::InvalidUri);
    assert(adapters::FileSource::open("file:///tmp/%00.mkv", encoded_source)
           == adapters::FileStatus::InvalidUri);
    assert(adapters::FileSource::open("file:///tmp/%xz.mkv", encoded_source)
           == adapters::FileStatus::InvalidUri);
    assert(adapters::FileSource::open("file:///this_path_does_not_exist_bdmv_xyz", encoded_source)
           == adapters::FileStatus::NotRegularFile);

    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) {
        workers.emplace_back([&source, t]() {
            std::array<std::byte, 32> output{};
            for (unsigned i = 0; i < 500; ++i) {
                const auto offset = (i * 43 + t * 157) % 4000;
                auto result = source->read_at(offset, output);
                assert(result.status == adapters::FileStatus::Ok && result.bytes_read == 32);
                assert(std::to_integer<unsigned>(output[0]) == (offset & 0xff));
            }
        });
    }
    for (auto& worker : workers) worker.join();
    source.reset();
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::remove(root, ec);
}

int main() {
    test_clock();
    test_timebase();
    test_epoch_queue();
    test_file();
    std::cout << "media foundations: file I/O + clock + timestamps + epoch queue passed\n";
}
