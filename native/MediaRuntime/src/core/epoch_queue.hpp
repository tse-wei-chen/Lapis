#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace bdmv::core {

enum class OverflowPolicy { RejectNewest, DropOldest };

template <class T> struct TimedPayload {
    std::uint64_t session_id{0};
    std::uint64_t epoch{0};
    std::int64_t pts_ns{0};
    T value;
};

// Non-blocking, bounded mailbox; strictly epoch-gated. Real-time producers
// never block on capacity. Consumer thread never sees stale seek frames.
template <class T> class EpochQueue final {
public:
    explicit EpochQueue(std::size_t capacity,
                        OverflowPolicy overflow = OverflowPolicy::RejectNewest)
        : capacity_(capacity), overflow_(overflow) {}

    void reset(std::uint64_t session_id, std::uint64_t epoch) {
        std::lock_guard lock(mutex_);
        session_id_ = session_id;
        epoch_ = epoch;
        pending_.clear();
        closed_ = false;
    }

    [[nodiscard]] bool try_push(TimedPayload<T> item) {
        std::lock_guard lock(mutex_);
        if (closed_ || capacity_ == 0 || item.session_id != session_id_ ||
            item.epoch != epoch_) return false;
        if (pending_.size() >= capacity_) {
            if (overflow_ == OverflowPolicy::RejectNewest) return false;
            pending_.pop_front();
        }
        pending_.push_back(std::move(item));
        return true;
    }

    [[nodiscard]] std::optional<TimedPayload<T>> try_pop() {
        std::lock_guard lock(mutex_);
        if (pending_.empty()) return std::nullopt;
        auto item = std::move(pending_.front());
        pending_.pop_front();
        return item;
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        closed_ = true;
        pending_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<TimedPayload<T>> pending_;
    std::size_t capacity_;
    OverflowPolicy overflow_;
    std::uint64_t session_id_{0};
    std::uint64_t epoch_{0};
    bool closed_{false};
};

} // namespace bdmv::core
