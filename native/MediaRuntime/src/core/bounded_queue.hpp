#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace lapis::media {

enum class QueueWriteStatus : std::uint8_t {
    Pushed,
    Full,
    Closed,
    Stopped
};

enum class QueueReadStatus : std::uint8_t {
    Item,
    Empty,
    Closed,
    Stopped
};

template <typename T>
struct QueueReadResult final {
    QueueReadStatus status{QueueReadStatus::Empty};
    std::optional<T> item;

    [[nodiscard]] bool has_item() const noexcept {
        return status == QueueReadStatus::Item && item.has_value();
    }
};

template <typename T>
class BoundedQueue final {
public:
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument(
                "BoundedQueue capacity must be greater than zero.");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    [[nodiscard]] QueueWriteStatus push(
        T value,
        std::stop_token stop_token = {}) {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return QueueWriteStatus::Closed;
        }
        if (stop_token.stop_requested()) {
            return QueueWriteStatus::Stopped;
        }

        const bool ready = not_full_.wait(
            lock,
            stop_token,
            [this] { return closed_ || queue_.size() < capacity_; });

        if (closed_) {
            return QueueWriteStatus::Closed;
        }
        if (!ready || stop_token.stop_requested()) {
            return QueueWriteStatus::Stopped;
        }

        queue_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return QueueWriteStatus::Pushed;
    }

    [[nodiscard]] QueueWriteStatus try_push(T value) {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return QueueWriteStatus::Closed;
        }
        if (queue_.size() >= capacity_) {
            return QueueWriteStatus::Full;
        }

        queue_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return QueueWriteStatus::Pushed;
    }

    [[nodiscard]] QueueReadResult<T> pop(
        std::stop_token stop_token = {}) {
        std::unique_lock lock(mutex_);
        if (stop_token.stop_requested()) {
            return {QueueReadStatus::Stopped, std::nullopt};
        }

        const bool ready = not_empty_.wait(
            lock,
            stop_token,
            [this] { return closed_ || !queue_.empty(); });

        if (!ready || stop_token.stop_requested()) {
            return {QueueReadStatus::Stopped, std::nullopt};
        }
        if (queue_.empty()) {
            return {QueueReadStatus::Closed, std::nullopt};
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return {QueueReadStatus::Item, std::move(value)};
    }

    [[nodiscard]] QueueReadResult<T> try_pop() {
        std::unique_lock lock(mutex_);
        if (queue_.empty()) {
            return {
                closed_ ? QueueReadStatus::Closed : QueueReadStatus::Empty,
                std::nullopt
            };
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return {QueueReadStatus::Item, std::move(value)};
    }

    [[nodiscard]] std::size_t clear() {
        std::size_t removed{};
        {
            std::scoped_lock lock(mutex_);
            removed = queue_.size();
            queue_.clear();
        }
        if (removed != 0) {
            not_full_.notify_all();
        }
        return removed;
    }

    void close() {
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] bool closed() const {
        std::scoped_lock lock(mutex_);
        return closed_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable_any not_empty_;
    std::condition_variable_any not_full_;
    std::deque<T> queue_;
    bool closed_{};
};

} // namespace lapis::media
