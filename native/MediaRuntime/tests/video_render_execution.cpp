#include "core/monotonic_time.hpp"
#include "core/playback_clock.hpp"
#include "execution/video_decode_worker.hpp"
#include "execution/video_render_worker.hpp"
#include "execution/video_scheduler_worker.hpp"
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
    lapis::media::VideoPresenterBeginStatus begin_status{
        lapis::media::VideoPresenterBeginStatus::Ready
    };
    lapis::media::VideoPresentStatus present_status{
        lapis::media::VideoPresentStatus::Presented
    };
    std::uint64_t begin_count{};
    std::uint64_t present_count{};
    std::uint64_t end_count{};
    lapis::media::PlaybackEpoch begun_epoch{};
    std::thread::id begin_thread{};
    std::thread::id end_thread{};
    std::vector<std::thread::id> present_threads;
    std::vector<lapis::media::VideoPresentContext> contexts;
    std::vector<std::uint32_t> widths;
    std::vector<std::uint32_t> heights;

    void reset_records() {
        begin_count = 0;
        present_count = 0;
        end_count = 0;
        begun_epoch = lapis::media::PlaybackEpoch{};
        begin_thread = {};
        end_thread = {};
        present_threads.clear();
        contexts.clear();
        widths.clear();
        heights.clear();
    }
};

class RecordingPresenter final : public lapis::media::IVideoPresenter {
public:
    explicit RecordingPresenter(std::shared_ptr<PresenterProbe> probe)
        : probe_(std::move(probe)) {}

    lapis::media::VideoPresenterBeginStatus begin(
        lapis::media::PlaybackEpoch epoch) override {
        ++probe_->begin_count;
        probe_->begun_epoch = epoch;
        probe_->begin_thread = std::this_thread::get_id();
        return probe_->begin_status;
    }

    lapis::media::VideoPresentStatus present(
        const lapis::media::VideoFrame& frame,
        const lapis::media::VideoPresentContext& context) override {
        ++probe_->present_count;
        probe_->present_threads.push_back(std::this_thread::get_id());
        probe_->contexts.push_back(context);
        probe_->widths.push_back(frame.surface != nullptr ? frame.surface->width() : 0);
        probe_->heights.push_back(frame.surface != nullptr ? frame.surface->height() : 0);
        return probe_->present_status;
    }

    void end() noexcept override {
        ++probe_->end_count;
        probe_->end_thread = std::this_thread::get_id();
    }

private:
    std::shared_ptr<PresenterProbe> probe_;
};

bool wait_for_render_completion(lapis::media::VideoRenderWorker& worker) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.state() != lapis::media::VideoRenderWorkerState::Running) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool wait_for_scheduler_completion(lapis::media::VideoSchedulerWorker& worker) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.state() != lapis::media::VideoSchedulerWorkerState::Running) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

int main() {
    using namespace lapis::media;

    const auto main_thread = std::this_thread::get_id();
    PlaybackMediaSession media;
    const MediaLocator locator{
        MediaLocatorKind::File,
        LAPIS_TEST_MULTI_FRAME_MEDIA_PATH
    };
    CHECK(media.open(locator).ok());

    PlaybackClock clock;
    const auto first_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{7}, MediaTime{}, first_now));
    CHECK(clock.start(MediaTime{}, first_now));

    VideoDecodeWorker decoder(media, 4);
    VideoSchedulerWorker scheduler(
        clock,
        VideoSchedulerConfig{
            MediaTime{1'000'000},
            MediaTime{2'000'000'000}
        },
        2);

    auto probe = std::make_shared<PresenterProbe>();
    VideoRenderWorker renderer(
        clock,
        std::make_unique<RecordingPresenter>(probe));

    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));
    auto decoded = decoder.start(PlaybackEpoch{7});
    auto presentation = scheduler.start(decoded);
    renderer.start(presentation);

    CHECK(wait_for_render_completion(renderer));
    renderer.stop();
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    CHECK(renderer.state() == VideoRenderWorkerState::Drained);
    auto render_stats = renderer.stats();
    CHECK(render_stats.presented_frames == 2);
    CHECK(render_stats.stale_epoch_drops == 0);
    CHECK(render_stats.inactive_clock_drops == 0);
    CHECK(render_stats.presenter_failures == 0);
    CHECK(probe->begin_count == 1);
    CHECK(probe->present_count == 2);
    CHECK(probe->end_count == 1);
    CHECK(probe->begun_epoch.value() == 7);
    CHECK(probe->begin_thread != main_thread);
    CHECK(probe->end_thread == probe->begin_thread);
    CHECK(probe->present_threads.size() == 2);
    CHECK(probe->present_threads[0] == probe->begin_thread);
    CHECK(probe->present_threads[1] == probe->begin_thread);
    CHECK(probe->contexts.size() == 2);
    CHECK(probe->contexts[0].epoch.value() == 7);
    CHECK(probe->contexts[1].epoch.value() == 7);
    CHECK(probe->widths[0] == 2 && probe->heights[0] == 2);
    CHECK(probe->widths[1] == 2 && probe->heights[1] == 2);

    CHECK(media.seek(MediaTime{}));
    const auto stale_source_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{8}, MediaTime{}, stale_source_now));
    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));

    auto stale_decoded = decoder.start(PlaybackEpoch{8});
    auto stale_presentation = scheduler.start(stale_decoded);
    CHECK(wait_for_scheduler_completion(scheduler));
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    const auto newer_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{9}, MediaTime{}, newer_now));
    probe->reset_records();
    renderer.start(stale_presentation);
    CHECK(wait_for_render_completion(renderer));
    renderer.stop();

    CHECK(renderer.state() == VideoRenderWorkerState::Drained);
    render_stats = renderer.stats();
    CHECK(render_stats.presented_frames == 0);
    CHECK(render_stats.stale_epoch_drops == 2);
    CHECK(render_stats.presenter_failures == 0);
    CHECK(probe->begin_count == 0);
    CHECK(probe->present_count == 0);
    CHECK(probe->end_count == 0);

    CHECK(media.seek(MediaTime{}));
    const auto failure_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{10}, MediaTime{}, failure_now));
    probe->reset_records();
    probe->present_status = VideoPresentStatus::DeviceLost;
    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));

    auto failure_decoded = decoder.start(PlaybackEpoch{10});
    auto failure_presentation = scheduler.start(failure_decoded);
    renderer.start(failure_presentation);
    CHECK(wait_for_render_completion(renderer));
    renderer.stop();
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    CHECK(renderer.state() == VideoRenderWorkerState::PresenterUnavailable);
    render_stats = renderer.stats();
    CHECK(render_stats.presented_frames == 0);
    CHECK(render_stats.presenter_failures == 1);
    CHECK(renderer.last_present_status() == VideoPresentStatus::DeviceLost);
    CHECK(probe->begin_count == 1);
    CHECK(probe->present_count == 1);
    CHECK(probe->end_count == 1);
    CHECK(probe->begin_thread == probe->present_threads[0]);
    CHECK(probe->end_thread == probe->begin_thread);

    return 0;
}
