#include "core/monotonic_time.hpp"
#include "core/playback_clock.hpp"
#include "execution/video_decode_worker.hpp"
#include "execution/video_scheduler_worker.hpp"
#include "session/playback_media_session.hpp"

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

lapis::media::QueueReadResult<lapis::media::ScheduledVideoFrame> wait_for_frame(
    lapis::media::VideoPresentationStream& stream) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 3s;

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

bool wait_for_closed(lapis::media::VideoPresentationStream& stream) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 3s;

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
    using namespace std::chrono_literals;

    PlaybackMediaSession media;
    const MediaLocator locator{
        MediaLocatorKind::File,
        LAPIS_TEST_MULTI_FRAME_MEDIA_PATH
    };
    const auto open_result = media.open(locator);
    CHECK(open_result.ok());
    CHECK(open_result.has_video);

    PlaybackClock clock;
    const auto initial_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{7}, MediaTime{}, initial_now));
    CHECK(clock.start(MediaTime{}, initial_now));

    VideoDecodeWorker decoder(media, 4);
    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));
    auto decoded = decoder.start(PlaybackEpoch{7});

    VideoSchedulerWorker scheduler(
        clock,
        VideoSchedulerConfig{
            MediaTime{1'000'000},
            MediaTime{2'000'000'000}
        },
        2);
    auto presentation = scheduler.start(decoded);

    auto first = wait_for_frame(*presentation);
    CHECK(first.has_item());
    CHECK(first.item->frame.epoch.value() == 7);
    CHECK(first.item->frame.timestamp == MediaTime{});
    CHECK(first.item->frame.payload.surface != nullptr);

    auto second = wait_for_frame(*presentation);
    CHECK(second.has_item());
    CHECK(second.item->frame.epoch.value() == 7);
    CHECK(second.item->frame.timestamp.nanoseconds > first.item->frame.timestamp.nanoseconds);
    CHECK(second.item->frame.payload.surface != nullptr);

    CHECK(wait_for_closed(*presentation));
    CHECK(scheduler.state() == VideoSchedulerWorkerState::Drained);
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    auto stats = scheduler.stats();
    CHECK(stats.released_frames == 2);
    CHECK(stats.stale_epoch_drops == 0);
    CHECK(stats.late_drops == 0);

    CHECK(media.seek(MediaTime{}));
    const auto stale_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{9}, MediaTime{}, stale_now));
    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));

    auto stale_decoded = decoder.start(PlaybackEpoch{8});
    auto stale_presentation = scheduler.start(stale_decoded);
    CHECK(wait_for_closed(*stale_presentation));
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    stats = scheduler.stats();
    CHECK(stats.released_frames == 0);
    CHECK(stats.stale_epoch_drops == 2);
    CHECK(stats.late_drops == 0);

    CHECK(media.seek(MediaTime{}));
    const auto paused_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{10}, MediaTime{}, paused_now));
    CHECK(clock.pause(paused_now));
    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));

    auto paused_decoded = decoder.start(PlaybackEpoch{10});
    auto paused_presentation = scheduler.start(paused_decoded);
    std::this_thread::sleep_for(10ms);
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    CHECK(scheduler.state() == VideoSchedulerWorkerState::Stopped);
    CHECK(paused_presentation->try_pop().status == QueueReadStatus::Closed);
    stats = scheduler.stats();
    CHECK(stats.released_frames == 0);

    return 0;
}
