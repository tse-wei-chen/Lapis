#include "audio/audio_output_provider.hpp"
#include "execution/playback_execution.hpp"
#include "process/ffmpeg/ffmpeg_audio_frame_converter.hpp"
#include "process/ffmpeg/ffmpeg_video_frame_converter.hpp"
#include "session/playback_media_session.hpp"
#include "video/video_presenter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

struct PresenterProbe final {
    std::uint64_t begin_count{};
    std::uint64_t present_count{};
    std::uint64_t end_count{};
    std::vector<std::uint64_t> begun_epochs;
    std::vector<std::uint64_t> presented_epochs;
    std::vector<lapis::media::VideoPixelFormat> pixel_formats;

    void reset_records() {
        begin_count = 0;
        present_count = 0;
        end_count = 0;
        begun_epochs.clear();
        presented_epochs.clear();
        pixel_formats.clear();
    }
};

class RecordingPresenter final : public lapis::media::IVideoPresenter {
public:
    explicit RecordingPresenter(std::shared_ptr<PresenterProbe> probe)
        : probe_(std::move(probe)) {}

    lapis::media::VideoPresenterBeginStatus begin(
        lapis::media::PlaybackEpoch epoch) override {
        ++probe_->begin_count;
        probe_->begun_epochs.push_back(epoch.value());
        return lapis::media::VideoPresenterBeginStatus::Ready;
    }

    lapis::media::VideoPresentStatus present(
        const lapis::media::VideoFrame& frame,
        const lapis::media::VideoPresentContext& context) override {
        ++probe_->present_count;
        probe_->presented_epochs.push_back(context.epoch.value());
        probe_->pixel_formats.push_back(
            frame.surface != nullptr
                ? frame.surface->pixel_format()
                : lapis::media::VideoPixelFormat::Unknown);
        return lapis::media::VideoPresentStatus::Presented;
    }

    void end() noexcept override {
        ++probe_->end_count;
    }

private:
    std::shared_ptr<PresenterProbe> probe_;
};

struct AudioOutputProbe final {
    std::atomic<std::uint64_t> prepare_count{};
    std::atomic<std::uint64_t> open_count{};
    std::atomic<std::uint64_t> submit_attempts{};
    std::atomic<std::uint64_t> accepted_sample_frames{};
    std::atomic<std::uint64_t> wait_count{};
    std::atomic<std::uint64_t> pause_count{};
    std::atomic<std::uint64_t> resume_count{};
    std::atomic<std::uint64_t> reset_count{};
    std::atomic<std::uint64_t> close_count{};
    std::atomic<bool> writable{};
    std::mutex wait_mutex;
    std::condition_variable_any wait_changed;

    void release_writes() {
        writable.store(true, std::memory_order_release);
        wait_changed.notify_all();
    }
};

class BlockingAudioOutput final : public lapis::media::IAudioOutput {
public:
    explicit BlockingAudioOutput(std::shared_ptr<AudioOutputProbe> probe)
        : probe_(std::move(probe)) {}

    [[nodiscard]] lapis::media::AudioOutputOpenStatus open(
        const lapis::media::AudioFormat&) override {
        probe_->open_count.fetch_add(1, std::memory_order_relaxed);
        return lapis::media::AudioOutputOpenStatus::Ready;
    }

    [[nodiscard]] lapis::media::AudioOutputSubmitResult try_submit(
        const lapis::media::AudioFrame& frame,
        std::uint32_t first_sample_frame) override {
        probe_->submit_attempts.fetch_add(1, std::memory_order_relaxed);
        if (frame.buffer == nullptr ||
            first_sample_frame >= frame.buffer->sample_frame_count()) {
            return {lapis::media::AudioOutputStatus::Faulted, 0};
        }
        if (!probe_->writable.load(std::memory_order_acquire)) {
            return {lapis::media::AudioOutputStatus::Backpressure, 0};
        }

        const auto accepted =
            frame.buffer->sample_frame_count() - first_sample_frame;
        probe_->accepted_sample_frames.fetch_add(
            accepted,
            std::memory_order_relaxed);
        return {lapis::media::AudioOutputStatus::Accepted, accepted};
    }

    [[nodiscard]] lapis::media::AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) override {
        probe_->wait_count.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(probe_->wait_mutex);
        const bool ready = probe_->wait_changed.wait(
            lock,
            stop_token,
            [this] {
                return probe_->writable.load(std::memory_order_acquire);
            });
        return ready
            ? lapis::media::AudioOutputWaitStatus::Ready
            : lapis::media::AudioOutputWaitStatus::Stopped;
    }

    [[nodiscard]] lapis::media::AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) override {
        return stop_token.stop_requested()
            ? lapis::media::AudioOutputWaitStatus::Stopped
            : lapis::media::AudioOutputWaitStatus::Ready;
    }

    [[nodiscard]] lapis::media::AudioOutputControlStatus pause() override {
        probe_->pause_count.fetch_add(1, std::memory_order_relaxed);
        return lapis::media::AudioOutputControlStatus::Ready;
    }

    [[nodiscard]] lapis::media::AudioOutputControlStatus resume() override {
        probe_->resume_count.fetch_add(1, std::memory_order_relaxed);
        return lapis::media::AudioOutputControlStatus::Ready;
    }

    void reset() noexcept override {
        probe_->reset_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] lapis::media::AudioOutputPosition position() const noexcept override {
        return {};
    }

    void close() noexcept override {
        probe_->close_count.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::shared_ptr<AudioOutputProbe> probe_;
};

class BlockingAudioOutputProvider final : public lapis::media::IAudioOutputProvider {
public:
    explicit BlockingAudioOutputProvider(std::shared_ptr<AudioOutputProbe> probe)
        : probe_(std::move(probe)) {}

    [[nodiscard]] lapis::media::AudioOutputPlan prepare_default_output() override {
        probe_->prepare_count.fetch_add(1, std::memory_order_relaxed);
        lapis::media::AudioOutputPlan plan;
        plan.status = lapis::media::AudioOutputPrepareStatus::Ready;
        plan.target_format = {
            lapis::media::AudioSampleFormat::S16,
            lapis::media::AudioPacking::Interleaved,
            48'000,
            {
                lapis::media::AudioChannel::FrontLeft,
                lapis::media::AudioChannel::FrontRight
            },
            false
        };
        plan.output = std::make_unique<BlockingAudioOutput>(probe_);
        return plan;
    }

private:
    std::shared_ptr<AudioOutputProbe> probe_;
};

bool wait_for_state(
    lapis::media::PlaybackExecution& execution,
    lapis::media::PlaybackExecutionState expected) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (execution.snapshot().state == expected) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool wait_for_count(
    const std::atomic<std::uint64_t>& value,
    std::uint64_t minimum) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

std::string audio_asset_path() {
    const std::filesystem::path video_path{LAPIS_TEST_PIPELINE_MEDIA_PATH};
    return (video_path.parent_path() / "test_audio.wav").string();
}

} // namespace

int main() {
    using namespace lapis::media;
    using namespace std::chrono_literals;

    // Existing video-only execution remains source-compatible and does not
    // accidentally activate audio when no audio dependencies are configured.
    {
        PlaybackMediaSession media;
        const MediaLocator locator{
            MediaLocatorKind::File,
            LAPIS_TEST_PIPELINE_MEDIA_PATH
        };
        const auto opened = media.open(locator);
        CHECK(opened.ok());
        CHECK(media.has_video());
        CHECK(!media.has_audio());

        auto probe = std::make_shared<PresenterProbe>();
        PlaybackExecution execution(
            media,
            std::make_unique<ffmpeg::FfmpegVideoFrameConverter>(),
            std::make_unique<RecordingPresenter>(probe),
            PlaybackExecutionConfig{
                1,
                1,
                1,
                VideoSchedulerConfig{
                    MediaTime{1'000'000},
                    MediaTime{2'000'000'000}
                }
            });

        auto snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Stopped);
        CHECK(snapshot.clock.epoch.value() == 0);

        CHECK(execution.start(PlaybackEpoch{1}, MediaTime{}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.video_active);
        CHECK(!snapshot.audio_active);
        CHECK(
            execution.seek(PlaybackEpoch{1}, MediaTime{}).error ==
            PlaybackExecutionError::StaleEpoch);

        CHECK(execution.pause().ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Paused);
        CHECK(snapshot.clock.state == PlaybackClockState::Paused);
        CHECK(snapshot.clock.epoch.value() == 1);

        // All three inter-stage queues have capacity 1. Pausing intentionally
        // creates backpressure through scheduler -> process -> decode. seek()
        // must still return because shutdown is downstream-to-upstream and every
        // bounded wait is stop-aware.
        std::this_thread::sleep_for(20ms);
        CHECK(execution.seek(PlaybackEpoch{2}, MediaTime{}).ok());

        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Paused);
        CHECK(snapshot.clock.state == PlaybackClockState::Paused);
        CHECK(snapshot.clock.epoch.value() == 2);

        probe->reset_records();
        std::this_thread::sleep_for(20ms);
        CHECK(probe->begin_count == 0);
        CHECK(probe->present_count == 0);

        CHECK(execution.resume().ok());
        CHECK(wait_for_state(execution, PlaybackExecutionState::Ended));

        snapshot = execution.snapshot();
        CHECK(snapshot.clock.epoch.value() == 2);
        CHECK(snapshot.process_state == VideoProcessWorkerState::Drained);
        CHECK(snapshot.process_status == VideoProcessStatus::Processed);
        CHECK(snapshot.rendering.presenter_failures == 0);
        CHECK(snapshot.rendering.stale_epoch_drops == 0);
        CHECK(snapshot.scheduling.stale_epoch_drops == 0);
        CHECK(probe->begin_count == 1);
        CHECK(probe->present_count == 16);
        CHECK(probe->end_count == 1);
        CHECK(probe->begun_epochs.size() == 1);
        CHECK(probe->begun_epochs[0] == 2);
        CHECK(probe->presented_epochs.size() == 16);
        CHECK(probe->pixel_formats.size() == 16);
        for (std::size_t index = 0; index < probe->presented_epochs.size(); ++index) {
            CHECK(probe->presented_epochs[index] == 2);
            CHECK(probe->pixel_formats[index] == VideoPixelFormat::Bgra8);
        }

        CHECK(
            execution.close(PlaybackEpoch{2}).error ==
            PlaybackExecutionError::StaleEpoch);
        CHECK(execution.close(PlaybackEpoch{3}).ok());

        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Stopped);
        CHECK(snapshot.clock.state == PlaybackClockState::Stopped);
        CHECK(snapshot.clock.epoch.value() == 3);
        CHECK(snapshot.clock.position == MediaTime{});
        CHECK(!snapshot.video_active);
        CHECK(!snapshot.audio_active);

        CHECK(
            execution.pause().error ==
            PlaybackExecutionError::InvalidState);
        CHECK(
            execution.start(PlaybackEpoch{4}, MediaTime{-1}).error ==
            PlaybackExecutionError::InvalidArgument);

        CHECK(execution.seek(PlaybackEpoch{4}, MediaTime{}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Stopped);
        CHECK(snapshot.clock.epoch.value() == 4);
        CHECK(snapshot.decode_state != VideoDecodeWorkerState::Running);
        CHECK(snapshot.process_state != VideoProcessWorkerState::Running);
        CHECK(snapshot.scheduler_state != VideoSchedulerWorkerState::Running);
        CHECK(snapshot.render_state != VideoRenderWorkerState::Running);
    }

    // Audio-only media exercises PlaybackExecution's optional audio branch
    // without requiring a physical endpoint. The fake output deliberately holds
    // backpressure so Pause/Resume and paused Seek boundaries are deterministic.
    {
        PlaybackMediaSession media;
        const MediaLocator locator{
            MediaLocatorKind::File,
            audio_asset_path()
        };
        const auto opened = media.open(locator);
        CHECK(opened.ok());
        CHECK(!media.has_video());
        CHECK(media.has_audio());

        auto output_probe = std::make_shared<AudioOutputProbe>();
        auto presenter_probe = std::make_shared<PresenterProbe>();

        PlaybackExecutionConfig config{};
        config.audio_decode_queue_capacity = 1;
        config.audio_process_queue_capacity = 1;

        PlaybackExecution execution(
            media,
            std::make_unique<ffmpeg::FfmpegVideoFrameConverter>(),
            std::make_unique<RecordingPresenter>(presenter_probe),
            std::make_unique<BlockingAudioOutputProvider>(output_probe),
            [](const AudioFormat& target) -> std::unique_ptr<IAudioFrameProcessor> {
                return std::make_unique<ffmpeg::FfmpegAudioFrameConverter>(target);
            },
            config);

        CHECK(execution.start(PlaybackEpoch{1}, MediaTime{}).ok());
        CHECK(wait_for_count(output_probe->wait_count, 1));

        auto snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Running);
        CHECK(!snapshot.video_active);
        CHECK(snapshot.audio_active);
        CHECK(snapshot.audio_render_state == AudioRenderWorkerState::Running);
        CHECK(output_probe->prepare_count.load(std::memory_order_relaxed) == 1);
        CHECK(output_probe->open_count.load(std::memory_order_relaxed) == 1);
        CHECK(presenter_probe->begin_count == 0);
        CHECK(presenter_probe->present_count == 0);

        CHECK(execution.pause().ok());
        CHECK(wait_for_count(output_probe->pause_count, 1));
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Paused);
        CHECK(snapshot.clock.state == PlaybackClockState::Paused);
        CHECK(snapshot.audio_render_state == AudioRenderWorkerState::Paused);

        CHECK(execution.resume().ok());
        CHECK(wait_for_count(output_probe->resume_count, 1));
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Running);
        CHECK(snapshot.clock.state == PlaybackClockState::Running);
        CHECK(snapshot.audio_render_state == AudioRenderWorkerState::Running);

        CHECK(execution.pause().ok());
        CHECK(wait_for_count(output_probe->pause_count, 2));
        CHECK(execution.seek(PlaybackEpoch{2}, MediaTime{}).ok());

        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Paused);
        CHECK(snapshot.clock.state == PlaybackClockState::Paused);
        CHECK(snapshot.clock.epoch.value() == 2);
        CHECK(snapshot.audio_active);
        CHECK(snapshot.audio_render_state == AudioRenderWorkerState::Idle);
        CHECK(output_probe->prepare_count.load(std::memory_order_relaxed) == 2);

        // The new generation may decode/process into bounded queues while paused,
        // but PlaybackExecution must not even open the replacement endpoint.
        std::this_thread::sleep_for(20ms);
        CHECK(output_probe->open_count.load(std::memory_order_relaxed) == 1);

        CHECK(execution.resume().ok());
        CHECK(wait_for_count(output_probe->open_count, 2));
        output_probe->release_writes();
        CHECK(wait_for_state(execution, PlaybackExecutionState::Ended));

        snapshot = execution.snapshot();
        CHECK(snapshot.clock.epoch.value() == 2);
        CHECK(snapshot.audio_decode_state == AudioDecodeWorkerState::Drained);
        CHECK(snapshot.audio_process_state == AudioProcessWorkerState::Drained);
        CHECK(snapshot.audio_process_status == AudioProcessStatus::Drained);
        CHECK(snapshot.audio_render_state == AudioRenderWorkerState::Drained);
        CHECK(snapshot.audio_rendering.submitted_frames > 0);
        CHECK(snapshot.audio_rendering.submitted_sample_frames > 0);
        CHECK(snapshot.audio_rendering.output_failures == 0);
        CHECK(output_probe->accepted_sample_frames.load(std::memory_order_relaxed) > 0);
        CHECK(presenter_probe->begin_count == 0);
        CHECK(presenter_probe->present_count == 0);

        CHECK(execution.close(PlaybackEpoch{3}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Stopped);
        CHECK(!snapshot.video_active);
        CHECK(!snapshot.audio_active);
        CHECK(output_probe->reset_count.load(std::memory_order_relaxed) >= 1);
        CHECK(output_probe->close_count.load(std::memory_order_relaxed) >= 2);
    }

    return 0;
}
