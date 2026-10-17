#include "core/playback_clock.hpp"
#include "video/video_scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main() {
    using namespace lapis::media;
    using namespace std::chrono_literals;

    constexpr std::int64_t Second = 1'000'000'000;
    PlaybackClock clock;
    const MonotonicTime start_time{10'000'000'000ULL};

    auto snapshot = clock.snapshot(start_time);
    CHECK(snapshot.state == PlaybackClockState::Stopped);
    CHECK(snapshot.epoch.value() == 0);
    CHECK(snapshot.revision == 0);

    CHECK(clock.rebase(PlaybackEpoch{1}, MediaTime{10 * Second}, start_time));
    snapshot = clock.snapshot(start_time);
    CHECK(snapshot.epoch.value() == 1);
    CHECK(snapshot.position == MediaTime{10 * Second});
    CHECK(snapshot.revision == 1);

    CHECK(clock.start(MediaTime{10 * Second}, start_time));
    CHECK(!clock.start(MediaTime{10 * Second}, start_time));

    snapshot = clock.snapshot(MonotonicTime{10'250'000'000ULL});
    CHECK(snapshot.state == PlaybackClockState::Running);
    CHECK(snapshot.position == MediaTime{10 * Second + 250'000'000});

    CHECK(clock.pause(MonotonicTime{10'300'000'000ULL}));
    CHECK(!clock.pause(MonotonicTime{10'400'000'000ULL}));
    snapshot = clock.snapshot(MonotonicTime{99'000'000'000ULL});
    CHECK(snapshot.state == PlaybackClockState::Paused);
    CHECK(snapshot.position == MediaTime{10 * Second + 300'000'000});

    CHECK(clock.rebase(
        PlaybackEpoch{2},
        MediaTime{20 * Second},
        MonotonicTime{99'000'000'000ULL}));
    snapshot = clock.snapshot(MonotonicTime{100'000'000'000ULL});
    CHECK(snapshot.state == PlaybackClockState::Paused);
    CHECK(snapshot.epoch.value() == 2);
    CHECK(snapshot.position == MediaTime{20 * Second});

    CHECK(clock.resume(MonotonicTime{100'000'000'000ULL}));
    snapshot = clock.snapshot(MonotonicTime{100'250'000'000ULL});
    CHECK(snapshot.position == MediaTime{20 * Second + 250'000'000});

    CHECK(clock.stop(MonotonicTime{100'500'000'000ULL}));
    CHECK(!clock.stop(MonotonicTime{100'600'000'000ULL}));
    snapshot = clock.snapshot(MonotonicTime{200'000'000'000ULL});
    CHECK(snapshot.state == PlaybackClockState::Stopped);
    CHECK(snapshot.position == MediaTime{20 * Second + 500'000'000});

    CHECK(clock.rebase(
        PlaybackEpoch{3},
        MediaTime{5 * Second},
        MonotonicTime{200'000'000'000ULL}));
    CHECK(clock.start(MediaTime{5 * Second}, MonotonicTime{201'000'000'000ULL}));
    snapshot = clock.snapshot(MonotonicTime{200'000'000'000ULL});
    CHECK(snapshot.epoch.value() == 3);
    CHECK(snapshot.position == MediaTime{5 * Second});

    clock.reset(PlaybackEpoch{4}, MonotonicTime{});
    snapshot = clock.snapshot(MonotonicTime{999});
    CHECK(snapshot.state == PlaybackClockState::Stopped);
    CHECK(snapshot.epoch.value() == 4);
    CHECK(snapshot.position == MediaTime{});
    CHECK(!clock.start(MediaTime{-1}, MonotonicTime{}));
    CHECK(!clock.rebase(PlaybackEpoch{5}, MediaTime{-1}, MonotonicTime{}));

    PlaybackClock saturating_clock;
    CHECK(saturating_clock.start(
        MediaTime{std::numeric_limits<std::int64_t>::max() - 10},
        MonotonicTime{}));
    CHECK(
        saturating_clock.snapshot(MonotonicTime{100}).position.nanoseconds ==
        std::numeric_limits<std::int64_t>::max());

    PlaybackClock notifying_clock;
    const auto observed_revision = notifying_clock.snapshot(MonotonicTime{}).revision;
    std::atomic<bool> waiter_entered{};
    std::atomic<PlaybackClockWaitStatus> wait_result{
        PlaybackClockWaitStatus::Timeout
    };

    std::jthread waiter([&](std::stop_token stop_token) {
        waiter_entered.store(true, std::memory_order_release);
        wait_result.store(
            notifying_clock.wait_for_change(observed_revision, stop_token),
            std::memory_order_release);
    });

    while (!waiter_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    CHECK(notifying_clock.rebase(
        PlaybackEpoch{9},
        MediaTime{123},
        MonotonicTime{456}));
    waiter.join();
    CHECK(
        wait_result.load(std::memory_order_acquire) ==
        PlaybackClockWaitStatus::Changed);

    const auto timeout_revision = notifying_clock.snapshot(MonotonicTime{}).revision;
    CHECK(
        notifying_clock.wait_for_change_for(
            timeout_revision,
            0ns) == PlaybackClockWaitStatus::Timeout);

    std::atomic<bool> stop_waiter_entered{};
    std::atomic<PlaybackClockWaitStatus> stop_wait_result{
        PlaybackClockWaitStatus::Timeout
    };
    std::jthread stop_waiter([&](std::stop_token stop_token) {
        stop_waiter_entered.store(true, std::memory_order_release);
        stop_wait_result.store(
            notifying_clock.wait_for_change(timeout_revision, stop_token),
            std::memory_order_release);
    });
    while (!stop_waiter_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    stop_waiter.request_stop();
    stop_waiter.join();
    CHECK(
        stop_wait_result.load(std::memory_order_acquire) ==
        PlaybackClockWaitStatus::Stopped);

    VideoScheduler scheduler;
    const PlaybackEpoch current_epoch{5};

    auto decision = scheduler.decide(
        current_epoch,
        MediaTime{1'020'000'000},
        current_epoch,
        MediaTime{1'000'000'000});
    CHECK(decision.action == VideoScheduleAction::Wait);
    CHECK(decision.drop_reason == VideoDropReason::None);
    CHECK(decision.frame_offset == MediaTime{20'000'000});
    CHECK(decision.wait_duration == MediaTime{18'000'000});

    decision = scheduler.decide(
        current_epoch,
        MediaTime{1'001'000'000},
        current_epoch,
        MediaTime{1'000'000'000});
    CHECK(decision.action == VideoScheduleAction::Present);

    decision = scheduler.decide(
        current_epoch,
        MediaTime{990'000'000},
        current_epoch,
        MediaTime{1'000'000'000});
    CHECK(decision.action == VideoScheduleAction::Present);
    CHECK(decision.frame_offset == MediaTime{-10'000'000});

    decision = scheduler.decide(
        current_epoch,
        MediaTime{940'000'000},
        current_epoch,
        MediaTime{1'000'000'000});
    CHECK(decision.action == VideoScheduleAction::Drop);
    CHECK(decision.drop_reason == VideoDropReason::TooLate);

    decision = scheduler.decide(
        PlaybackEpoch{4},
        MediaTime{1'000'000'000},
        current_epoch,
        MediaTime{1'000'000'000});
    CHECK(decision.action == VideoScheduleAction::Drop);
    CHECK(decision.drop_reason == VideoDropReason::StaleEpoch);

    decision = scheduler.decide(
        current_epoch,
        MediaTime{std::numeric_limits<std::int64_t>::min()},
        current_epoch,
        MediaTime{std::numeric_limits<std::int64_t>::max()});
    CHECK(decision.frame_offset.nanoseconds == std::numeric_limits<std::int64_t>::min());
    CHECK(decision.action == VideoScheduleAction::Drop);

    bool invalid_config_rejected = false;
    try {
        [[maybe_unused]] const VideoScheduler invalid_scheduler(
            VideoSchedulerConfig{MediaTime{-1}, MediaTime{50'000'000}});
    } catch (const std::invalid_argument&) {
        invalid_config_rejected = true;
    }
    CHECK(invalid_config_rejected);

    return 0;
}
