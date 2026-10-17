#include "core/bounded_queue.hpp"
#include "execution/video_decode_worker.hpp"
#include "session/playback_media_session.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

lapis::media::QueueReadResult<lapis::media::TimedVideoFrame> wait_for_frame(
    lapis::media::VideoFrameStream& stream) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 2s;

    for (;;) {
        auto result = stream.try_pop();
        if (result.status != lapis::media::QueueReadStatus::Empty) {
            return result;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return result;
        }
        std::this_thread::sleep_for(1ms);
    }
}

bool wait_for_closed(lapis::media::VideoFrameStream& stream) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 2s;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = stream.try_pop();
        if (result.status == lapis::media::QueueReadStatus::Closed) {
            return true;
        }
        if (result.status == lapis::media::QueueReadStatus::Item) {
            continue;
        }
        std::this_thread::sleep_for(1ms);
    }

    return false;
}

} // namespace

int main() {
    using namespace lapis::media;

    BoundedQueue<int> queue(1);
    CHECK(queue.capacity() == 1);
    CHECK(queue.try_push(10) == QueueWriteStatus::Pushed);
    CHECK(queue.try_push(20) == QueueWriteStatus::Full);

    std::atomic<bool> producer_entered{};
    std::atomic<QueueWriteStatus> blocked_result{QueueWriteStatus::Pushed};
    std::jthread blocked_producer(
        [&](std::stop_token stop_token) {
            producer_entered.store(true, std::memory_order_release);
            blocked_result.store(
                queue.push(20, stop_token),
                std::memory_order_release);
        });

    while (!producer_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    blocked_producer.request_stop();
    blocked_producer.join();
    CHECK(blocked_result.load(std::memory_order_acquire) == QueueWriteStatus::Stopped);

    auto queued_value = queue.try_pop();
    CHECK(queued_value.has_item());
    CHECK(*queued_value.item == 10);

    queue.close();
    CHECK(queue.try_push(30) == QueueWriteStatus::Closed);
    CHECK(queue.try_pop().status == QueueReadStatus::Closed);

    PlaybackMediaSession media;
    const MediaLocator locator{MediaLocatorKind::File, LAPIS_TEST_MEDIA_PATH};
    const auto open_result = media.open(locator);
    CHECK(open_result.ok());
    CHECK(open_result.has_video);

    VideoDecodeWorker worker(media, 1);
    auto first_stream = worker.start(PlaybackEpoch{7});
    CHECK(first_stream->epoch().value() == 7);
    CHECK(first_stream->capacity() == 1);

    auto first_frame = wait_for_frame(*first_stream);
    CHECK(first_frame.has_item());
    CHECK(first_frame.item->epoch.value() == 7);
    CHECK(first_frame.item->timestamp == first_frame.item->payload.timestamp);
    CHECK(first_frame.item->payload.surface != nullptr);
    CHECK(first_frame.item->payload.surface->width() == 2);
    CHECK(first_frame.item->payload.surface->height() == 2);
    CHECK(wait_for_closed(*first_stream));
    CHECK(worker.state() == VideoDecodeWorkerState::Drained);
    worker.stop();

    CHECK(media.seek(MediaTime{0}));

    auto second_stream = worker.start(PlaybackEpoch{8});
    CHECK(first_stream->try_pop().status == QueueReadStatus::Closed);

    auto second_frame = wait_for_frame(*second_stream);
    CHECK(second_frame.has_item());
    CHECK(second_frame.item->epoch.value() == 8);
    CHECK(second_frame.item->timestamp == second_frame.item->payload.timestamp);
    worker.stop();

    return 0;
}
