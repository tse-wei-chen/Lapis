#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace lapis::media {

// Diagnostic event IDs form a separate opt-in wire protocol. Do not reorder.
enum class DiagnosticEventKind : std::uint32_t {
    DeviceFailure = 1,
    DeviceCreated = 2,
    SwapChainCreated = 3,
    SwapChainResized = 4,
    RecoveryAttempted = 5,
    RecoveryBindSucceeded = 6,
    RecoveryBindFailed = 7,
    RecoveryPresented = 8,
    RecoveryTerminated = 9,
    RecoveryIncompleteAtEof = 10
};

struct DiagnosticEvent final {
    std::uint64_t sequence{};
    std::uint64_t epoch{};
    std::uint64_t monotonic_milliseconds{};
    DiagnosticEventKind kind{};
    std::uint32_t failure_stage{};
    std::uint32_t hresult{};
    std::uint32_t removed_reason{};
    std::uint64_t detail{};
};

// Session-scoped, fixed-size, no retained paths, no allocations at record time.
// Read operations copy under a mutex and never invoke GPU or disk I/O.
class DiagnosticEventJournal final {
public:
    static constexpr std::uint32_t capacity = 128;

    void record(
        std::uint64_t epoch,
        DiagnosticEventKind kind,
        std::uint32_t stage = 0,
        std::uint32_t hresult = 0,
        std::uint32_t removed_reason = 0,
        std::uint64_t detail = 0) noexcept {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto timestamp = std::chrono::duration_cast<
            std::chrono::milliseconds>(now).count();
        std::scoped_lock lock(mutex_);
        const auto sequence = ++sequence_;
        events_[(sequence - 1) % capacity] = {
            sequence, epoch, static_cast<std::uint64_t>(timestamp),
            kind, stage, hresult, removed_reason, detail
        };
    }

    // Cursor is EXCLUSIVE. next_cursor is the last returned sequence, not
    // the newest retained event if the caller's destination is too small.
    // dropped counts overwritten unread events. Zero-capacity reads are valid.
    [[nodiscard]] std::uint32_t read_after(
        std::uint64_t cursor,
        DiagnosticEvent* output,
        std::uint32_t output_capacity,
        std::uint64_t& next_cursor,
        std::uint64_t& dropped) const noexcept {
        std::scoped_lock lock(mutex_);
        next_cursor = cursor;
        dropped = 0;
        const auto oldest = sequence_ >= capacity
            ? sequence_ - capacity + 1
            : 1;
        if (cursor >= sequence_) {
            return 0;
        }

        // Avoid overflow at UINT64_MAX; the early check above handles the
        // only realistic wrap-adjacent cursor case for this session.
        const auto next = cursor + 1;
        if (next < oldest) {
            dropped = oldest - next;
        }
        const auto start = std::max(oldest, next);
        if (output_capacity == 0 || output == nullptr) {
            return 0;
        }
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                sequence_ - start + 1, output_capacity));
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto sequence = start + i;
            output[i] = events_[(sequence - 1) % capacity];
        }
        if (count != 0) {
            next_cursor = start + count - 1;
        }
        return count;
    }

private:
    mutable std::mutex mutex_;
    std::array<DiagnosticEvent, capacity> events_{};
    std::uint64_t sequence_{};
};

} // namespace lapis::media
