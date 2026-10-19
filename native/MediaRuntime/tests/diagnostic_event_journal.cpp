#include "core/diagnostic_events.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main() {
    using namespace lapis::media;
    DiagnosticEventJournal journal;
    DiagnosticEvent output[DiagnosticEventJournal::capacity]{};
    std::uint64_t cursor{};
    std::uint64_t lost{};

    CHECK(journal.read_after(0, output, 128, cursor, lost) == 0);
    CHECK(cursor == 0 && lost == 0);
    for (std::uint64_t i = 1; i <= 300; ++i) {
        journal.record(i / 10, DiagnosticEventKind::DeviceFailure,
            9, 0x887A0005, 0x887A0006, i);
    }
    CHECK(journal.read_after(0, output, 128, cursor, lost) == 128);
    CHECK(lost == 172);
    CHECK(cursor == 300);
    CHECK(output[0].sequence == 173);
    CHECK(output[0].detail == 173);
    CHECK(output[127].sequence == 300);
    CHECK(output[127].epoch == 30);
    CHECK(output[127].hresult == 0x887A0005);
    CHECK(journal.read_after(cursor, output, 128, cursor, lost) == 0);
    CHECK(lost == 0);

    // Pagination must not jump over unread records when capacity is small.
    std::uint64_t next{};
    CHECK(journal.read_after(170, output, 5, next, lost) == 5);
    CHECK(lost == 2);
    CHECK(next == 177);
    CHECK(journal.read_after(next, output, 5, cursor, lost) == 5);
    CHECK(lost == 0);
    CHECK(output[0].sequence == 178);
    CHECK(cursor == 182);
    CHECK(journal.read_after(cursor, nullptr, 0, next, lost) == 0);
    CHECK(next == cursor);

    // Writers from different threads may interleave, but each read returns
    // ordered contiguous sequence numbers and bounded memory.
    DiagnosticEventJournal concurrent;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&concurrent, i] {
            for (int j = 0; j < 500; ++j) {
                concurrent.record(static_cast<std::uint64_t>(i + 1),
                    DiagnosticEventKind::SwapChainResized);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(concurrent.read_after(0, output, 128, cursor, lost) == 128);
    CHECK(lost == 1872);
    CHECK(cursor == 2000);
    for (std::uint64_t i = 1; i < 128; ++i) {
        CHECK(output[i].sequence == output[i - 1].sequence + 1);
    }
    return 0;
}
