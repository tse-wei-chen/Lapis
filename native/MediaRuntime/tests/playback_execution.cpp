#include "audio/audio_output_provider.hpp"
#include "execution/playback_execution.hpp"
#include "process/ffmpeg/ffmpeg_audio_frame_converter.hpp"
#include "process/ffmpeg/ffmpeg_video_frame_converter.hpp"
#include "session/playback_media_session.hpp"
#include "source/bluray/bluray_source.hpp"
#include "source/file/file_source.hpp"
#include "video/video_presenter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
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

// Device loss is injected at the IAudioOutput seam only in this native
// regression test. A fresh endpoint is selected when the user closes and
// reopens the playback generation; we do not claim seamless auto-recovery.
struct EndpointCycleProbe final {
    std::atomic<std::uint64_t> prepared{};
    std::atomic<std::uint64_t> opened{};
    std::atomic<std::uint64_t> lost_submits{};
    std::atomic<std::uint64_t> successful_submits{};
    std::atomic<std::uint64_t> reset_calls{};
    std::atomic<std::uint64_t> close_calls{};
};

class CyclingAudioOutput final : public lapis::media::IAudioOutput {
public:
    CyclingAudioOutput(
        std::shared_ptr<EndpointCycleProbe> probe, bool fail_this_generation)
        : probe_(std::move(probe)),
          fail_this_generation_(fail_this_generation) {}

    lapis::media::AudioOutputOpenStatus open(
        const lapis::media::AudioFormat&) override {
        probe_->opened.fetch_add(1, std::memory_order_relaxed);
        return lapis::media::AudioOutputOpenStatus::Ready;
    }

    lapis::media::AudioOutputSubmitResult try_submit(
        const lapis::media::AudioFrame& frame,
        std::uint32_t first) override {
        if (fail_this_generation_) {
            probe_->lost_submits.fetch_add(1, std::memory_order_relaxed);
            return {lapis::media::AudioOutputStatus::DeviceLost, 0};
        }
        if (frame.buffer == nullptr ||
            first >= frame.buffer->sample_frame_count()) {
            return {lapis::media::AudioOutputStatus::Faulted, 0};
        }
        probe_->successful_submits.fetch_add(1, std::memory_order_relaxed);
        return {
            lapis::media::AudioOutputStatus::Accepted,
            frame.buffer->sample_frame_count() - first
        };
    }

    lapis::media::AudioOutputWaitStatus wait_until_writable(
        std::stop_token token) override {
        return token.stop_requested()
            ? lapis::media::AudioOutputWaitStatus::Stopped
            : lapis::media::AudioOutputWaitStatus::Ready;
    }

    lapis::media::AudioOutputWaitStatus wait_until_drained(
        std::stop_token token) override {
        return token.stop_requested()
            ? lapis::media::AudioOutputWaitStatus::Stopped
            : lapis::media::AudioOutputWaitStatus::Ready;
    }

    lapis::media::AudioOutputControlStatus pause() override {
        return lapis::media::AudioOutputControlStatus::Ready;
    }
    lapis::media::AudioOutputControlStatus resume() override {
        return lapis::media::AudioOutputControlStatus::Ready;
    }
    void reset() noexcept override {
        probe_->reset_calls.fetch_add(1, std::memory_order_relaxed);
    }
    lapis::media::AudioOutputPosition position() const noexcept override {
        return {};
    }
    void close() noexcept override {
        probe_->close_calls.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::shared_ptr<EndpointCycleProbe> probe_;
    const bool fail_this_generation_;
};

class CyclingEndpointProvider final : public lapis::media::IAudioOutputProvider {
public:
    explicit CyclingEndpointProvider(std::shared_ptr<EndpointCycleProbe> probe)
        : probe_(std::move(probe)) {}

    lapis::media::AudioOutputPlan prepare_default_output() override {
        const auto generation = probe_->prepared.fetch_add(
            1, std::memory_order_relaxed) + 1;
        lapis::media::AudioOutputPlan plan{};
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
        plan.output = std::make_unique<CyclingAudioOutput>(
            probe_, generation == 1);
        return plan;
    }

private:
    std::shared_ptr<EndpointCycleProbe> probe_;
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

bool wait_for_clock_position(
    const lapis::media::PlaybackExecution& execution,
    lapis::media::MediaTime minimum) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (execution.snapshot().clock.position.nanoseconds >= minimum.nanoseconds) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

std::filesystem::path filesystem_path_from_utf8(const std::string& value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const unsigned char character : value) {
        utf8.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(utf8);
}

std::string playback_asset_path() {
    const auto utf8_path = filesystem_path_from_utf8(
        LAPIS_TEST_CHURN_MEDIA_PATH).u8string();
    return {
        reinterpret_cast<const char*>(utf8_path.data()),
        utf8_path.size()
    };
}

std::string av_playback_asset_path() {
    const auto utf8_path = filesystem_path_from_utf8(
        LAPIS_TEST_AV_MEDIA_PATH).u8string();
    return {
        reinterpret_cast<const char*>(utf8_path.data()),
        utf8_path.size()
    };
}

std::string audio_asset_path() {
    const auto video_path = filesystem_path_from_utf8(LAPIS_TEST_PIPELINE_MEDIA_PATH);
    const auto utf8_path = (video_path.parent_path() / "test_audio.wav").u8string();
    return {
        reinterpret_cast<const char*>(utf8_path.data()),
        utf8_path.size()
    };
}

std::string bluray_asset_path() {
    const auto utf8_path = filesystem_path_from_utf8(
        LAPIS_TEST_BLURAY_MEDIA_PATH).u8string();
    return {
        reinterpret_cast<const char*>(utf8_path.data()),
        utf8_path.size()
    };
}

class TestBlurayDisc final : public lapis::media::bluray::IBlurayDisc {
public:
    explicit TestBlurayDisc(const std::filesystem::path& media_path)
        : stream_(media_path) {}

    [[nodiscard]] lapis::media::NavigationSnapshot navigation() const override {
        using namespace lapis::media;
        NavigationSnapshot snapshot;
        snapshot.revision = 1;
        snapshot.capabilities = NavigationCapability::Titles |
                                NavigationCapability::Playlists;
        TitleInfo title;
        title.id = 0;
        title.playlist_id = 1;
        title.duration = MediaTime{35'000'000'000};
        snapshot.titles.push_back(title);
        snapshot.playlists.push_back({1, title.duration});
        snapshot.selected_title = 0;
        snapshot.selected_playlist = 1;
        return snapshot;
    }

    lapis::media::StreamReadResult read(
        std::span<std::byte> bytes) override {
        return stream_.read(bytes);
    }

    lapis::media::StreamSeekResult seek(
        std::int64_t offset,
        lapis::media::SeekOrigin origin) override {
        return stream_.seek(offset, origin);
    }

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return stream_.size().value_or(0);
    }

private:
    lapis::media::FileByteStream stream_;
};

class TemporaryBdmvRoot final {
public:
    TemporaryBdmvRoot()
        : root_(std::filesystem::temp_directory_path() /
            ("lapis-bdmv-playback-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(root_ / "BDMV" / "PLAYLIST");
        std::filesystem::create_directories(root_ / "BDMV" / "STREAM");
        std::ofstream(root_ / "BDMV" / "index.bdmv").put('B');
    }

    ~TemporaryBdmvRoot() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

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
        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, true).ok());
        CHECK(execution.snapshot().state == PlaybackExecutionState::Running);
        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, false).ok());
        CHECK(execution.snapshot().state == PlaybackExecutionState::Running);
        snapshot = execution.snapshot();
        CHECK(snapshot.video_active);
        CHECK(!snapshot.audio_active);
        const auto initial_video_plan = execution.active_graph_plan();
        CHECK(initial_video_plan.has_value());
        CHECK(initial_video_plan->contains(GraphNodeKind::VideoSink));
        CHECK(!initial_video_plan->contains(GraphNodeKind::AudioSink));
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
        const auto replacement_video_plan = execution.active_graph_plan();
        CHECK(replacement_video_plan.has_value());
        CHECK(replacement_video_plan->contains(GraphNodeKind::VideoSink));

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
        CHECK(!execution.active_graph_plan().has_value());

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

    // Exercise discontinuities and temporary rate changes against a
    // multi-second compressed file while decode and presentation are active.
    {
        PlaybackMediaSession media;
        const MediaLocator locator{
            MediaLocatorKind::File,
            playback_asset_path()
        };
        const auto opened = media.open(locator);
        CHECK(opened.ok());
        CHECK(opened.has_video);
        CHECK(opened.duration.has_value());
        CHECK(opened.duration->nanoseconds >= 5'000'000'000);

        auto probe = std::make_shared<PresenterProbe>();
        PlaybackExecution execution(
            media,
            std::make_unique<ffmpeg::FfmpegVideoFrameConverter>(),
            std::make_unique<RecordingPresenter>(probe));
        CHECK(execution.start(PlaybackEpoch{1}, MediaTime{}).ok());
        CHECK(wait_for_clock_position(execution, MediaTime{150'000'000}));

        const auto before_fast_forward = execution.snapshot().clock.position;
        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, true).ok());
        const auto fast_forward_started = std::chrono::steady_clock::now();
        CHECK(wait_for_clock_position(
            execution,
            MediaTime{before_fast_forward.nanoseconds + 400'000'000}));
        CHECK(
            std::chrono::steady_clock::now() - fast_forward_started <
            350ms);
        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, false).ok());
        CHECK(execution.snapshot().state == PlaybackExecutionState::Running);

        CHECK(execution.seek(
            PlaybackEpoch{2}, MediaTime{2'000'000'000}).ok());
        auto snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Running);
        CHECK(snapshot.clock.position.nanoseconds >= 2'000'000'000);

        CHECK(execution.pause().ok());
        CHECK(execution.seek(
            PlaybackEpoch{3}, MediaTime{4'000'000'000}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Paused);
        CHECK(snapshot.clock.position.nanoseconds == 4'000'000'000);

        CHECK(execution.resume().ok());
        CHECK(wait_for_state(execution, PlaybackExecutionState::Ended));
        snapshot = execution.snapshot();
        CHECK(snapshot.clock.epoch.value() == 3);
        CHECK(snapshot.rendering.presenter_failures == 0);
        CHECK(probe->present_count > 0);
        CHECK(std::find(
            probe->presented_epochs.begin(),
            probe->presented_epochs.end(),
            3) != probe->presented_epochs.end());
        for (std::size_t index = 0; index < probe->presented_epochs.size(); ++index) {
            CHECK(probe->pixel_formats[index] == VideoPixelFormat::Bgra8);
        }
    }

    // Exercise timestamp seeks and temporary fast-forward through the actual
    // Blu-ray source adapter, MPEG-TS demux hint and custom FFmpeg AVIO path.
    // The fake disc supplies synthetic M2TS bytes; this is not a claim of real
    // libbluray/hardware acceptance.
    {
        TemporaryBdmvRoot bdmv;
        const auto media_path = filesystem_path_from_utf8(bluray_asset_path());
        PlaybackMediaSession media;
        media.register_source_provider(
            std::make_unique<bluray::BluraySourceProvider>(
                [media_path](const std::filesystem::path&) {
                    return std::make_unique<TestBlurayDisc>(media_path);
                }));

        const auto root_bytes = bdmv.path().u8string();
        std::string root_path;
        root_path.reserve(root_bytes.size());
        for (const char8_t character : root_bytes) {
            root_path.push_back(static_cast<char>(character));
        }
        const auto opened = media.open({
            MediaLocatorKind::Directory, std::move(root_path)
        });
        CHECK(opened.ok());
        CHECK(opened.has_video);
        CHECK(opened.duration.has_value());
        CHECK(opened.duration->nanoseconds >= 35'000'000'000);

        auto probe = std::make_shared<PresenterProbe>();
        PlaybackExecution execution(
            media,
            std::make_unique<ffmpeg::FfmpegVideoFrameConverter>(),
            std::make_unique<RecordingPresenter>(probe));
        CHECK(execution.start(PlaybackEpoch{1}, MediaTime{}).ok());
        CHECK(wait_for_clock_position(execution, MediaTime{100'000'000}));

        const auto before_fast_forward =
            execution.snapshot().clock.position.nanoseconds;
        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, true).ok());
        const auto fast_forward_started = std::chrono::steady_clock::now();
        CHECK(wait_for_clock_position(
            execution, MediaTime{before_fast_forward + 300'000'000}));
        CHECK(std::chrono::steady_clock::now() - fast_forward_started < 300ms);
        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, false).ok());
        const auto before_forward_seek =
            execution.snapshot().clock.position.nanoseconds;
        const auto forward_seek_target =
            before_forward_seek + 10'000'000'000;
        CHECK(execution.seek(
            PlaybackEpoch{2}, MediaTime{forward_seek_target}).ok());
        auto snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Running);
        CHECK(snapshot.clock.epoch.value() == 2);
        CHECK(snapshot.clock.position.nanoseconds >= forward_seek_target);
        CHECK(snapshot.clock.position.nanoseconds <
              forward_seek_target + 1'000'000'000);
        CHECK(execution.seek(
            PlaybackEpoch{3},
            MediaTime{forward_seek_target - 10'000'000'000}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Running);
        CHECK(snapshot.clock.epoch.value() == 3);
        const auto backward_seek_target =
            forward_seek_target - 10'000'000'000;
        CHECK(snapshot.clock.position.nanoseconds >= backward_seek_target);
        CHECK(snapshot.clock.position.nanoseconds <
              backward_seek_target + 1'000'000'000);

        CHECK(execution.pause().ok());
        CHECK(execution.seek(
            PlaybackEpoch{4}, MediaTime{20'000'000'000}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Paused);
        CHECK(snapshot.clock.epoch.value() == 4);
        CHECK(snapshot.clock.position.nanoseconds == 20'000'000'000);

        CHECK(execution.resume().ok());
        CHECK(execution.seek(
            PlaybackEpoch{5}, MediaTime{29'500'000'000}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Running);
        CHECK(snapshot.clock.epoch.value() == 5);
        CHECK(snapshot.clock.position.nanoseconds >= 29'500'000'000);
        CHECK(wait_for_clock_position(execution, MediaTime{29'600'000'000}));
        CHECK(execution.snapshot().clock.epoch.value() == 5);
        CHECK(execution.close(PlaybackEpoch{6}).ok());
    }

    // Returning to normal speed must start a fresh audio render generation;
    // fast-forward intentionally retires the old audio graph.
    {
        PlaybackMediaSession media;
        const MediaLocator locator{
            MediaLocatorKind::File,
            av_playback_asset_path()
        };
        const auto opened = media.open(locator);
        CHECK(opened.ok());
        CHECK(opened.has_video);
        CHECK(opened.has_audio);

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
        CHECK(wait_for_count(output_probe->open_count, 1));
        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, true).ok());
        CHECK(wait_for_count(output_probe->close_count, 1));
        CHECK(!execution.snapshot().audio_active);

        CHECK(execution.set_fast_forward(PlaybackEpoch{1}, false).ok());
        CHECK(wait_for_count(output_probe->open_count, 2));
        auto snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Running);
        CHECK(snapshot.audio_active);
        CHECK(execution.close(PlaybackEpoch{2}).ok());
        CHECK(output_probe->close_count.load(std::memory_order_relaxed) >= 2);
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
        const auto audio_plan = execution.active_graph_plan();
        CHECK(audio_plan.has_value());
        CHECK(audio_plan->contains(GraphNodeKind::AudioSink));
        CHECK(!audio_plan->contains(GraphNodeKind::VideoSink));
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

    // A WASAPI-equivalent device loss during PCM Submit must surface as a
    // terminal fault, not silent EOF. The caller can close, release the
    // invalidated endpoint, and start a new epoch on a freshly chosen device.
    {
        PlaybackMediaSession media;
        const MediaLocator source{
            MediaLocatorKind::File,
            audio_asset_path()
        };
        CHECK(media.open(source).ok());
        CHECK(media.has_audio());
        auto probe = std::make_shared<EndpointCycleProbe>();
        auto video_probe = std::make_shared<PresenterProbe>();

        PlaybackExecution execution(
            media,
            std::make_unique<ffmpeg::FfmpegVideoFrameConverter>(),
            std::make_unique<RecordingPresenter>(video_probe),
            std::make_unique<CyclingEndpointProvider>(probe),
            [](const AudioFormat& format)
                -> std::unique_ptr<IAudioFrameProcessor> {
                return std::make_unique<ffmpeg::FfmpegAudioFrameConverter>(
                    format);
            });

        CHECK(execution.start(PlaybackEpoch{101}, MediaTime{}).ok());
        CHECK(wait_for_state(execution, PlaybackExecutionState::Faulted));
        auto snapshot = execution.snapshot();
        CHECK(snapshot.clock.epoch.value() == 101);
        CHECK(snapshot.audio_active);
        CHECK(snapshot.audio_render_state ==
            AudioRenderWorkerState::OutputUnavailable);
        CHECK(snapshot.audio_rendering.output_failures == 1);
        CHECK(probe->prepared.load() == 1);
        CHECK(probe->opened.load() == 1);
        CHECK(probe->lost_submits.load() == 1);
        CHECK(probe->successful_submits.load() == 0);

        CHECK(execution.close(PlaybackEpoch{102}).ok());
        snapshot = execution.snapshot();
        CHECK(snapshot.state == PlaybackExecutionState::Stopped);
        CHECK(!snapshot.audio_active);
        CHECK(probe->reset_calls.load() == 1);
        CHECK(probe->close_calls.load() == 1);

        // The transport owner reopens from the beginning after choosing a
        // replacement audio endpoint. All new samples belong to epoch 103.
        CHECK(media.seek(MediaTime{}));
        CHECK(execution.start(PlaybackEpoch{103}, MediaTime{}).ok());
        CHECK(wait_for_state(execution, PlaybackExecutionState::Ended));
        snapshot = execution.snapshot();
        CHECK(snapshot.clock.epoch.value() == 103);
        CHECK(snapshot.audio_render_state == AudioRenderWorkerState::Drained);
        CHECK(snapshot.audio_rendering.output_failures == 0);
        CHECK(snapshot.audio_rendering.submitted_sample_frames > 0);
        CHECK(probe->prepared.load() == 2);
        CHECK(probe->opened.load() == 2);
        CHECK(probe->lost_submits.load() == 1);
        CHECK(probe->successful_submits.load() > 0);
        CHECK(execution.close(PlaybackEpoch{104}).ok());
        CHECK(probe->close_calls.load() == 2);
    }

    return 0;
}
