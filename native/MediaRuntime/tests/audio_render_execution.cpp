#include "audio/audio_output.hpp"
#include "execution/audio_decode_worker.hpp"
#include "execution/audio_render_worker.hpp"
#include "session/playback_media_session.hpp"

#include <algorithm>
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
    PartialChunks,
    BlockUntilStopped,
    PauseBetweenChunks,
    PauseDuringDrain,
    SerializedControls,
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

    lapis::media::AudioOutputSubmitResult try_submit(
        const lapis::media::AudioFrame& frame,
        std::uint32_t first_sample_frame) override {
        submit_attempts.fetch_add(1, std::memory_order_relaxed);
        if (first_sample_frame != 0) {
            saw_nonzero_offset.store(true, std::memory_order_release);
        }
        if (mode_ == OutputMode::DeviceLost) {
            return {lapis::media::AudioOutputStatus::DeviceLost, 0};
        }
        if (mode_ == OutputMode::BlockUntilStopped ||
            mode_ == OutputMode::SerializedControls) {
            return {lapis::media::AudioOutputStatus::Backpressure, 0};
        }
        if (mode_ == OutputMode::BackpressureOnce &&
            !returned_backpressure_.exchange(true, std::memory_order_acq_rel)) {
            return {lapis::media::AudioOutputStatus::Backpressure, 0};
        }

        const auto total = frame.buffer->sample_frame_count();
        if (first_sample_frame >= total) {
            return {lapis::media::AudioOutputStatus::Faulted, 0};
        }
        const auto remaining = total - first_sample_frame;
        const auto accepted =
            mode_ == OutputMode::PartialChunks ||
            mode_ == OutputMode::PauseBetweenChunks
            ? std::min<std::uint32_t>(remaining, 128U)
            : remaining;

        accepted_sample_frames.fetch_add(accepted, std::memory_order_relaxed);
        if (accepted == remaining) {
            accepted_frames.fetch_add(1, std::memory_order_relaxed);
        }
        return {lapis::media::AudioOutputStatus::Accepted, accepted};
    }

    lapis::media::AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) override {
        writable_waits.fetch_add(1, std::memory_order_relaxed);
        if (mode_ != OutputMode::BlockUntilStopped &&
            mode_ != OutputMode::PauseBetweenChunks &&
            mode_ != OutputMode::SerializedControls) {
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
        if (mode_ != OutputMode::PauseDuringDrain) {
            return stop_token.stop_requested()
                ? lapis::media::AudioOutputWaitStatus::Stopped
                : lapis::media::AudioOutputWaitStatus::Ready;
        }

        drain_started.store(true, std::memory_order_release);
        std::unique_lock lock(wait_mutex_);
        const bool ready = wait_cv_.wait(
            lock,
            stop_token,
            [this] { return drain_ready_; });
        return ready
            ? lapis::media::AudioOutputWaitStatus::Ready
            : lapis::media::AudioOutputWaitStatus::Stopped;
    }

    lapis::media::AudioOutputControlStatus pause() override {
        pause_count.fetch_add(1, std::memory_order_relaxed);
        if (mode_ == OutputMode::SerializedControls) {
            pause_started.store(true, std::memory_order_release);
            std::unique_lock lock(control_mutex_);
            (void)control_cv_.wait_for(
                lock,
                std::chrono::seconds(4),
                [this] { return allow_pause_; });
        }
        return lapis::media::AudioOutputControlStatus::Ready;
    }

    lapis::media::AudioOutputControlStatus resume() override {
        resume_count.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock lock(wait_mutex_);
            if (mode_ == OutputMode::PauseBetweenChunks) {
                writable_ = true;
            }
            if (mode_ == OutputMode::PauseDuringDrain) {
                drain_ready_ = true;
            }
        }
        wait_cv_.notify_all();
        return lapis::media::AudioOutputControlStatus::Ready;
    }

    void release_pause_control() {
        {
            std::scoped_lock lock(control_mutex_);
            allow_pause_ = true;
        }
        control_cv_.notify_all();
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
    std::atomic<std::uint64_t> pause_count{};
    std::atomic<std::uint64_t> resume_count{};
    std::atomic<std::uint64_t> reset_count{};
    std::atomic<std::uint64_t> close_count{};
    std::atomic<bool> wait_started{};
    std::atomic<bool> drain_started{};
    std::atomic<bool> pause_started{};
    std::atomic<bool> saw_nonzero_offset{};

private:
    OutputMode mode_;
    std::atomic<bool> returned_backpressure_{};
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;
    bool writable_{};
    bool drain_ready_{};
    std::mutex control_mutex_;
    std::condition_variable control_cv_;
    bool allow_pause_{};
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
        CHECK(stats.writable_waits == 1);
        CHECK(stats.output_failures == 0);
        CHECK(probe->open_count.load() == 1);
        CHECK(probe->accepted_sample_frames.load() == 480);
        CHECK(probe->writable_waits.load() == 1);
        CHECK(probe->drain_waits.load() == 1);
        CHECK(probe->reset_count.load() == 0);
        CHECK(probe->close_count.load() == 1);
    }

    // A device buffer smaller than a decoded frame must make forward progress
    // through accepted prefixes instead of entering permanent backpressure.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{22});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::PartialChunks);
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
        CHECK(stats.writable_waits >= 1);
        CHECK(stats.output_failures == 0);
        CHECK(probe->accepted_sample_frames.load() == 480);
        CHECK(probe->saw_nonzero_offset.load(std::memory_order_acquire));
        CHECK(probe->writable_waits.load() >= 1);
        CHECK(probe->drain_waits.load() == 1);
    }

    // Pause must interrupt a device readiness wait without terminating the
    // generation. Resume continues the same worker and Stop remains destructive.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{23});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::BlockUntilStopped);
        auto* probe = output.get();
        AudioRenderWorker renderer(std::move(output));
        renderer.start(stream);

        CHECK(wait_for_flag(probe->wait_started));
        CHECK(renderer.pause());
        CHECK(renderer.state() == AudioRenderWorkerState::Paused);
        CHECK(probe->pause_count.load() == 1);
        CHECK(probe->reset_count.load() == 0);
        CHECK(probe->close_count.load() == 0);

        CHECK(renderer.resume());
        CHECK(renderer.state() == AudioRenderWorkerState::Running);
        CHECK(probe->resume_count.load() == 1);

        renderer.stop();
        CHECK(renderer.state() == AudioRenderWorkerState::Stopped);
        decoder.stop();
        media.stop_decode_generation();

        CHECK(probe->reset_count.load() == 1);
        CHECK(probe->close_count.load() == 1);
    }

    // Concurrent synchronous controls must be ordered. A Resume issued while
    // Pause is still being applied cannot supersede the Pause revision or lose
    // the wakeup that Resume needs after Pause has acknowledged.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{24});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::SerializedControls);
        auto* probe = output.get();
        AudioRenderWorker renderer(std::move(output));
        renderer.start(stream);

        CHECK(wait_for_flag(probe->wait_started));

        std::atomic<bool> pause_result{};
        std::atomic<bool> pause_done{};
        std::jthread pause_thread([&] {
            pause_result.store(renderer.pause(), std::memory_order_release);
            pause_done.store(true, std::memory_order_release);
        });
        CHECK(wait_for_flag(probe->pause_started));

        std::atomic<bool> resume_started{};
        std::atomic<bool> resume_result{};
        std::atomic<bool> resume_done{};
        std::jthread resume_thread([&] {
            resume_started.store(true, std::memory_order_release);
            resume_result.store(renderer.resume(), std::memory_order_release);
            resume_done.store(true, std::memory_order_release);
        });
        CHECK(wait_for_flag(resume_started));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!resume_done.load(std::memory_order_acquire));

        probe->release_pause_control();
        const bool pause_completed = wait_for_flag(pause_done);
        const bool resume_completed = wait_for_flag(resume_done);
        if (!pause_completed || !resume_completed) {
            renderer.stop();
        }
        pause_thread.join();
        resume_thread.join();

        CHECK(pause_completed);
        CHECK(resume_completed);
        CHECK(pause_result.load(std::memory_order_acquire));
        CHECK(resume_result.load(std::memory_order_acquire));
        CHECK(renderer.state() == AudioRenderWorkerState::Running);
        CHECK(probe->pause_count.load() == 1);
        CHECK(probe->resume_count.load() == 1);

        renderer.stop();
        decoder.stop();
        media.stop_decode_generation();
        CHECK(probe->reset_count.load() == 1);
        CHECK(probe->close_count.load() == 1);
    }

    // A pause that interrupts the wait between partial prefixes must preserve
    // the current frame cursor. Resume submits only the remaining suffix.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{25});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::PauseBetweenChunks);
        auto* probe = output.get();
        AudioRenderWorker renderer(std::move(output));
        renderer.start(stream);

        CHECK(wait_for_flag(probe->wait_started));
        CHECK(probe->accepted_sample_frames.load() == 128);
        CHECK(renderer.pause());
        CHECK(renderer.state() == AudioRenderWorkerState::Paused);
        CHECK(renderer.resume());
        CHECK(wait_for_state(renderer, AudioRenderWorkerState::Drained));

        renderer.stop();
        decoder.stop();
        media.stop_decode_generation();

        CHECK(renderer.stats().submitted_sample_frames == 480);
        CHECK(probe->accepted_sample_frames.load() == 480);
        CHECK(probe->pause_count.load() == 1);
        CHECK(probe->resume_count.load() == 1);
        CHECK(probe->saw_nonzero_offset.load(std::memory_order_acquire));
        CHECK(probe->reset_count.load() == 0);
        CHECK(probe->close_count.load() == 1);
    }

    // EOF drain is also interruptible by Pause. Resume returns to the same drain
    // phase rather than reading the already-closed input again or resetting audio.
    {
        PlaybackMediaSession media;
        CHECK(open_audio_media(media));
        AudioDecodeWorker decoder(media, 1);
        auto stream = decoder.start(PlaybackEpoch{26});

        auto output = std::make_unique<RecordingAudioOutput>(
            OutputMode::PauseDuringDrain);
        auto* probe = output.get();
        AudioRenderWorker renderer(std::move(output));
        renderer.start(stream);

        CHECK(wait_for_flag(probe->drain_started));
        CHECK(renderer.pause());
        CHECK(renderer.state() == AudioRenderWorkerState::Paused);
        CHECK(renderer.resume());
        CHECK(wait_for_state(renderer, AudioRenderWorkerState::Drained));

        renderer.stop();
        decoder.stop();
        media.stop_decode_generation();

        CHECK(probe->pause_count.load() == 1);
        CHECK(probe->resume_count.load() == 1);
        CHECK(probe->drain_waits.load() >= 2);
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
        auto stream = decoder.start(PlaybackEpoch{27});

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
        auto stream = decoder.start(PlaybackEpoch{28});

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
