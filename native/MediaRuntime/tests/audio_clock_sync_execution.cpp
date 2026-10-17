#include "core/monotonic_time.hpp"
#include "core/playback_clock.hpp"
#include "execution/audio_clock_sync_worker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

bool wait_for_applied(
    lapis::media::AudioClockSyncWorker& worker,
    std::uint64_t minimum) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.stats().rebases_applied >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool wait_for_maintained(lapis::media::AudioClockSyncWorker& worker) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.stats().maintained > 0) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool wait_for_ignored(
    lapis::media::AudioClockSyncWorker& worker,
    std::uint64_t minimum) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.stats().ignored >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

class SyntheticAudioClock final {
public:
    SyntheticAudioClock(
        lapis::media::MonotonicTime origin,
        std::int64_t offset_nanoseconds,
        std::uint64_t revision)
        : origin_(origin),
          offset_nanoseconds_(offset_nanoseconds),
          revision_(revision) {}

    lapis::media::AudioOutputPosition read() const noexcept {
        const auto now = lapis::media::SteadyClockSource::now();
        const auto elapsed = now.nanoseconds >= origin_.nanoseconds
            ? now.nanoseconds - origin_.nanoseconds
            : 0U;
        const auto maximum = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
        const auto bounded_elapsed = elapsed > maximum
            ? maximum
            : elapsed;
        const auto elapsed_ns = static_cast<std::int64_t>(bounded_elapsed);
        const auto offset = offset_nanoseconds_.load(std::memory_order_acquire);
        const auto headroom = std::numeric_limits<std::int64_t>::max() - offset;
        const auto position = elapsed_ns > headroom
            ? std::numeric_limits<std::int64_t>::max()
            : offset + elapsed_ns;

        return {
            lapis::media::MediaTime{position},
            now,
            revision_.load(std::memory_order_acquire),
            source_.load(std::memory_order_acquire)
        };
    }

    void set_offset(std::int64_t value) noexcept {
        offset_nanoseconds_.store(value, std::memory_order_release);
    }

    void set_source(lapis::media::AudioOutputPositionSource source) noexcept {
        source_.store(source, std::memory_order_release);
    }

private:
    lapis::media::MonotonicTime origin_{};
    std::atomic<std::int64_t> offset_nanoseconds_{};
    std::atomic<std::uint64_t> revision_{};
    std::atomic<lapis::media::AudioOutputPositionSource> source_{
        lapis::media::AudioOutputPositionSource::HardwareClock
    };
};

} // namespace

int main() {
    using namespace lapis::media;
    using namespace std::chrono_literals;

    // Compare-and-apply rebases may not cross a logical clock transition.
    {
        PlaybackClock clock;
        const MonotonicTime t0{1'000'000'000};
        CHECK(clock.rebase(PlaybackEpoch{7}, MediaTime{}, t0));
        CHECK(clock.start(MediaTime{}, t0));

        const auto expected = clock.snapshot(MonotonicTime{1'010'000'000});
        CHECK(expected.state == PlaybackClockState::Running);
        CHECK(clock.pause(MonotonicTime{1'020'000'000}));

        CHECK(!clock.try_rebase(
            expected,
            MediaTime{500'000'000},
            MonotonicTime{1'020'000'000}));
        const auto paused = clock.snapshot(MonotonicTime{1'500'000'000});
        CHECK(paused.state == PlaybackClockState::Paused);
        CHECK(paused.epoch.value() == 7);
        CHECK(paused.position == MediaTime{20'000'000});

        CHECK(clock.resume(MonotonicTime{1'500'000'000}));
        const auto current = clock.snapshot(MonotonicTime{1'500'000'000});
        CHECK(clock.try_rebase(
            current,
            MediaTime{125'000'000},
            MonotonicTime{1'500'000'000}));
        const auto corrected = clock.snapshot(MonotonicTime{1'500'000'000});
        CHECK(corrected.state == PlaybackClockState::Running);
        CHECK(corrected.epoch.value() == 7);
        CHECK(corrected.position == MediaTime{125'000'000});
        CHECK(corrected.revision == current.revision + 1);

        CHECK(!clock.try_rebase(
            corrected,
            MediaTime{-1},
            MonotonicTime{1'500'000'000}));
    }

    // A hardware source outside the deadband steers PlaybackClock once, then
    // settles into Maintain. Paused clocks are not polled/corrected; Resume may
    // apply a fresh correction if hardware moved while logical time was frozen.
    {
        PlaybackClock clock;
        const auto origin = SteadyClockSource::now();
        CHECK(clock.rebase(PlaybackEpoch{11}, MediaTime{}, origin));
        CHECK(clock.start(MediaTime{}, origin));

        SyntheticAudioClock source(origin, 80'000'000, 41);
        AudioClockSyncConfig config;
        config.sample_interval = MediaTime{2'000'000};
        config.discipline.rebase_threshold = MediaTime{5'000'000};
        config.discipline.maximum_observation_age = MediaTime{100'000'000};

        AudioClockSyncWorker worker(
            clock,
            [&source] { return source.read(); },
            config);
        worker.start();

        CHECK(wait_for_applied(worker, 1));
        CHECK(wait_for_maintained(worker));
        auto stats = worker.stats();
        CHECK(stats.evaluations >= 2);
        CHECK(stats.rebase_recommendations >= 1);
        CHECK(stats.rebases_applied >= 1);
        CHECK(stats.rebases_rejected == 0);
        CHECK(stats.last_output_revision == 41);

        CHECK(clock.pause(SteadyClockSource::now()));
        const auto paused_snapshot = clock.snapshot(SteadyClockSource::now());
        const auto applied_while_running = worker.stats().rebases_applied;
        source.set_offset(350'000'000);
        std::this_thread::sleep_for(20ms);

        const auto still_paused = clock.snapshot(SteadyClockSource::now());
        CHECK(still_paused.state == PlaybackClockState::Paused);
        CHECK(still_paused.position == paused_snapshot.position);
        CHECK(worker.stats().rebases_applied == applied_while_running);

        CHECK(clock.resume(SteadyClockSource::now()));
        CHECK(wait_for_applied(worker, applied_while_running + 1));

        source.set_source(AudioOutputPositionSource::Estimated);
        const auto ignored_before = worker.stats().ignored;
        const auto applied_before_estimated = worker.stats().rebases_applied;
        CHECK(wait_for_ignored(worker, ignored_before + 1));
        std::this_thread::sleep_for(5ms);
        CHECK(worker.stats().rebases_applied == applied_before_estimated);

        worker.stop();
        CHECK(worker.state() == AudioClockSyncWorkerState::Stopped);
    }

    return 0;
}
