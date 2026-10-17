#include "audio/audio_output.hpp"
#include "execution/audio_decode_worker.hpp"
#include "execution/audio_render_worker.hpp"
#include "session/playback_media_session.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

enum class OutputMode {
    BackpressureOnce,
    BlockUntilStopped,
    DeviceLost
};

class RecordingAudioOutput final : public lapis::media::IAudioOutput {
public:
    explicit RecordingAudioOutput(OutputMode mode) : mode_(mode) {}

    lapis::media::AudioOutputOpenStatus open(
        const lapis::media::AudioFormat&) override {
        open_count.fetch_add(1, std::memory_order_relaxed);
        return lapis::media::AudioOutputOpenStatus::Ready;
    }

    lapis::media::AudioOutputStatus try_submit(
        const lapis::media::AudioFrame& frame) override {
        submit_attempts.fetch_add(1, std::memory_order_relaxed);
        if (mode_ == OutputMode::DeviceLost) {
            return lapis::media::AudioOutputStatus::DeviceLost;
        }
        if (mode_ == OutputMode::BlockUntilStopped) {
            return lapis::media::AudioOutputStatus::Backpressure;
        }
        if (!returned_backpressure_.exchange(true, std::memory_order_acq_rel)) {
            return lapis::media::AudioOutputStatus::Backpressure;
        }

        accepted_frames.fetch_add(1, std::memory_order_relaxed);
        accepted_sample_frames.fetch_add(
            frame.buffer->sample_frame_count(),
            std::memory_order_relaxed);
        return lapis::media::AudioOutputStatus::Accepted;
    }

    lapis::media::AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) override {
        writable_waits.fetch_add(1, std::memory_order_relaxed);
        if (mode_ != OutputMode::BlockUntilStopped) {
            return lapis::media::AudioOutputWaitStatus::Ready;
        }

        wait_started.store(true, std::memory_order_release);
        std::unique_lock lock(wait_mutex_);
        const bool ready = wait_cv_.wait(
            lock,
            stop_token,
            [this] { return writable_; });
        return ready
            ? lapis::media::AudioOutputWaitStatus::Ready
            : lapis::media::AudioOutputWaitStatus::Stopped;
    }

    lapis::media::AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) override {
        drain_waits.fetch_add(1, std::memory_order_relaxed);
        return stop_token.stop_requested()
            ? lapis::media::AudioOutputWaitStatus::Stopped
            : lapis::media::AudioOutputWaitStatus::Ready;
    }

    void reset() noexcept override {
        reset_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] lapis::media::AudioOutputPosition position() const noexcept override {
        return {};
    }

    void close() noexcept override {
        close_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<std::uint64_t> open_count{};
    std::atomic<std::uint64_t> submit_attempts{};
    std::atomic<std::uint64_t> accepted_frames{};
    std::atomic<std::uint64_t> accepted_sample_frames{};
    std::atomic<std::uint64_t> writable_waits{};
    std::atomic<std::uint64_t> drain_waits{};
    std::atomic<std::uint64_t> reset_count{};
    std::atomic<std::uint64_t> close_count{};
    std::atomic<bool> wait_started{};

private:
    OutputMode mode_;
    std::atomic<bool> returned_backpressure_{};
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;
    bool writable_{};
};

bool wait_for_state(
    lapis::media::AudioRenderWorker& worker,
    lapis::media::AudioRenderWorkerState expected) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.state() == expected) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool wait_for_flag(const std::atomic<bool>& flag) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool open_audio_media(lapis::media::PlaybackMediaSession& media) {
    const lapis::media::MediaLocator locator{
        lapis::media::MediaLocatorKind::File,
        LAPIS_TEST_AUDIO_PATH
    };
    const auto opened = media.open(locator);
    return opened.ok() && opened.has_audio &&
        media.start_decode_generation({false, true});
}

} // namespace

int main() {
    using namespace lapis::media;

    // Normal EOF must wait through output drain. A transient Backpressure is
    // handled through the output readiness primitive without polling.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{21});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::BackpressureOnce);
        auto* probe = output.get();
        AudioRenderWorker renderer(std::move(output));
        renderer.start(stream);

        CHECK(wait_for_state(renderer, AudioRenderWorkerState::Drained));
        renderer.stop();
        decoder.stop();
        media.stop_decode_generation();

        const auto stats = renderer.stats();
        CHECK(stats.submitted_frames > 0);
        CHECK(stats.submitted_sample_frames == 480);
        CHECK(stats.backpressure_waits == 1);
        CHECK(stats.output_failures == 0);
        CHECK(probe->open_count.load() == 1);
        CHECK(probe->accepted_sample_frames.load() == 480);
        CHECK(probe->writable_waits.load() == 1);
        CHECK(probe->drain_waits.load() == 1);
        CHECK(probe->reset_count.load() == 0);
        CHECK(probe->close_count.load() == 1);
    }

    // A seek/close-style stop must interrupt an output readiness wait and reset
    // queued device samples before close. The upstream decoder can then be
    // stopped even if it was backpressured by its bounded stream.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{22});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::BlockUntilStopped);
        auto* probe = output.get();
        AudioRenderWorker renderer(std::move(output));
        renderer.start(stream);

        CHECK(wait_for_flag(probe->wait_started));
        renderer.stop();
        CHECK(renderer.state() == AudioRenderWorkerState::Stopped);
        decoder.stop();
        media.stop_decode_generation();

        CHECK(probe->writable_waits.load() == 1);
        CHECK(probe->reset_count.load() == 1);
        CHECK(probe->close_count.load() == 1);
    }

    // Device loss is distinguished from malformed data and from caller stop so
    // the future session owner can trigger device recovery rather than poisoning
    // the decoder generation.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{23});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::DeviceLost);
        auto* probe = output.get();
        AudioRenderWorker renderer(std::move(output));
        renderer.start(stream);

        CHECK(wait_for_state(renderer, AudioRenderWorkerState::OutputUnavailable));
        renderer.stop();
        decoder.stop();
        media.stop_decode_generation();

        CHECK(renderer.stats().output_failures == 1);
        CHECK(renderer.last_submit_status() == AudioOutputStatus::DeviceLost);
        CHECK(probe->reset_count.load() == 1);
        CHECK(probe->close_count.load() == 1);
    }

    return 0;
}
