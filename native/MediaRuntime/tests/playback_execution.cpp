#include "execution/playback_execution.hpp"
#include "session/playback_media_session.hpp"
#include "video/video_presenter.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
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

    void reset_records() {
        begin_count = 0;
        present_count = 0;
        end_count = 0;
        begun_epochs.clear();
        presented_epochs.clear();
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
        const lapis::media::VideoFrame&,
        const lapis::media::VideoPresentContext& context) override {
        ++probe_->present_count;
        probe_->presented_epochs.push_back(context.epoch.value());
        return lapis::media::VideoPresentStatus::Presented;
    }

    void end() noexcept override {
        ++probe_->end_count;
    }

private:
    std::shared_ptr<PresenterProbe> probe_;
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

} // namespace

int main() {
    using namespace lapis::media;
    using namespace std::chrono_literals;

    PlaybackMediaSession media;
    const MediaLocator locator{
        MediaLocatorKind::File,
        LAPIS_TEST_PIPELINE_MEDIA_PATH
    };
    CHECK(media.open(locator).ok());

    auto probe = std::make_shared<PresenterProbe>();
    PlaybackExecution execution(
        media,
        std::make_unique<RecordingPresenter>(probe),
        PlaybackExecutionConfig{
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
    CHECK(
        execution.seek(PlaybackEpoch{1}, MediaTime{}).error ==
        PlaybackExecutionError::StaleEpoch);

    CHECK(execution.pause().ok());
    snapshot = execution.snapshot();
    CHECK(snapshot.state == PlaybackExecutionState::Paused);
    CHECK(snapshot.clock.state == PlaybackClockState::Paused);
    CHECK(snapshot.clock.epoch.value() == 1);

    // With both queues at capacity 1, a paused clock leaves the scheduler
    // waiting and quickly backpressures the decoder. seek() must still return:
    // Render -> Scheduler -> Decode stop ordering wakes every blocked stage.
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
    CHECK(snapshot.rendering.presenter_failures == 0);
    CHECK(snapshot.rendering.stale_epoch_drops == 0);
    CHECK(snapshot.scheduling.stale_epoch_drops == 0);
    CHECK(probe->begin_count == 1);
    CHECK(probe->present_count == 16);
    CHECK(probe->end_count == 1);
    CHECK(probe->begun_epochs.size() == 1);
    CHECK(probe->begun_epochs[0] == 2);
    CHECK(probe->presented_epochs.size() == 16);
    for (const auto epoch : probe->presented_epochs) {
        CHECK(epoch == 2);
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
    CHECK(snapshot.scheduler_state != VideoSchedulerWorkerState::Running);
    CHECK(snapshot.render_state != VideoRenderWorkerState::Running);

    return 0;
}
