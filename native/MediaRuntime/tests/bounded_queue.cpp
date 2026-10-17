#include "core/bounded_queue.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <stop_token>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "CHECK failed: " #condition << " at " << __LINE__ << '\n'; \
    return 1; \
} } while (false)

int main() {
    using namespace lapis::media;
    using namespace std::chrono_literals;

    // Capacity is a hard bound and FIFO ordering is preserved.
    BoundedQueue<int> queue(2);
    CHECK(queue.capacity() == 2);
    CHECK(queue.try_pop().status == QueueReadStatus::Empty);
    CHECK(queue.try_push(10) == QueueWriteStatus::Pushed);
    CHECK(queue.try_push(20) == QueueWriteStatus::Pushed);
    CHECK(queue.try_push(30) == QueueWriteStatus::Full);
    CHECK(queue.size() == 2);
    auto first = queue.try_pop();
    CHECK(first.has_item() && first.item.value() == 10);
    auto second = queue.pop();
    CHECK(second.has_item() && second.item.value() == 20);

    // Closing retains buffered items, then returns Closed, and rejects writes.
    CHECK(queue.try_push(40) == QueueWriteStatus::Pushed);
    queue.close();
    queue.close();
    CHECK(queue.closed());
    CHECK(queue.try_push(50) == QueueWriteStatus::Closed);
    CHECK(queue.push(50) == QueueWriteStatus::Closed);
    auto remaining = queue.pop();
    CHECK(remaining.has_item() && remaining.item.value() == 40);
    CHECK(queue.pop().status == QueueReadStatus::Closed);
    CHECK(queue.try_pop().status == QueueReadStatus::Closed);

    // A producer blocked by backpressure must wake when its queue closes.
    BoundedQueue<int> full(1);
    CHECK(full.push(1) == QueueWriteStatus::Pushed);
    std::promise<void> producer_entered;
    auto producer_ready = producer_entered.get_future();
    auto producer = std::async(std::launch::async, [&] {
        producer_entered.set_value();
        return full.push(2);
    });
    producer_ready.wait();
    CHECK(producer.wait_for(10ms) == std::future_status::timeout);
    full.close();
    CHECK(producer.wait_for(2s) == std::future_status::ready);
    CHECK(producer.get() == QueueWriteStatus::Closed);

    // A blocked consumer must wake when its queue closes.
    BoundedQueue<int> empty(1);
    std::promise<void> consumer_entered;
    auto consumer_ready = consumer_entered.get_future();
    auto consumer = std::async(std::launch::async, [&] {
        consumer_entered.set_value();
        return empty.pop();
    });
    consumer_ready.wait();
    CHECK(consumer.wait_for(10ms) == std::future_status::timeout);
    empty.close();
    CHECK(consumer.wait_for(2s) == std::future_status::ready);
    CHECK(consumer.get().status == QueueReadStatus::Closed);

    // Stop requests interrupt blocked reads and writes without closing queues.
    BoundedQueue<int> stopped_read(1);
    std::stop_source read_stop;
    auto reader = std::async(std::launch::async, [&] {
        return stopped_read.pop(read_stop.get_token());
    });
    CHECK(reader.wait_for(10ms) == std::future_status::timeout);
    read_stop.request_stop();
    CHECK(reader.wait_for(2s) == std::future_status::ready);
    CHECK(reader.get().status == QueueReadStatus::Stopped);
    CHECK(!stopped_read.closed());

    BoundedQueue<int> stopped_write(1);
    CHECK(stopped_write.push(1) == QueueWriteStatus::Pushed);
    std::stop_source write_stop;
    auto writer = std::async(std::launch::async, [&] {
        return stopped_write.push(2, write_stop.get_token());
    });
    CHECK(writer.wait_for(10ms) == std::future_status::timeout);
    write_stop.request_stop();
    CHECK(writer.wait_for(2s) == std::future_status::ready);
    CHECK(writer.get() == QueueWriteStatus::Stopped);
    CHECK(stopped_write.size() == 1);

    // Clearing a full queue releases a blocked producer.
    BoundedQueue<int> cleared(1);
    CHECK(cleared.push(1) == QueueWriteStatus::Pushed);
    auto waiting_writer = std::async(std::launch::async, [&] {
        return cleared.push(2);
    });
    CHECK(waiting_writer.wait_for(10ms) == std::future_status::timeout);
    CHECK(cleared.clear() == 1);
    CHECK(waiting_writer.wait_for(2s) == std::future_status::ready);
    CHECK(waiting_writer.get() == QueueWriteStatus::Pushed);
    CHECK(cleared.pop().item.value() == 2);

    return 0;
}
