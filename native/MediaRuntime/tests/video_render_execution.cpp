#include "core/monotonic_time.hpp"
#include "core/playback_clock.hpp"
#include "execution/video_decode_worker.hpp"
#include "execution/video_render_worker.hpp"
#include "execution/video_scheduler_worker.hpp"
#include "session/playback_media_session.hpp"
#include "video/video_presenter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
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
    lapis::media::VideoOutputSurfaceApplyStatus surface_status{
        lapis::media::VideoOutputSurfaceApplyStatus::Ready
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
    std::vector<lapis::media::VideoOutputSurfaceDescriptor> surfaces;
    std::vector<std::thread::id> surface_threads;

    std::mutex present_gate_mutex;
    std::condition_variable present_gate_changed;
    bool block_first_present{};
    bool device_lost_once{};
    bool device_always_lost{};
    bool first_present_entered{};
    bool release_first_present{};

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
        surfaces.clear();
        surface_threads.clear();
        block_first_present = false;
        device_lost_once = false;
        device_always_lost = false;
        first_present_entered = false;
        release_first_present = false;
    }
};

class RecordingPresenter final : public lapis::media::IVideoPresenter {
public:
    explicit RecordingPresenter(std::shared_ptr<PresenterProbe> probe)
        : probe_(std::move(probe)) {}

    lapis::media::VideoOutputSurfaceApplyStatus set_output_surface(
        const lapis::media::VideoOutputSurfaceDescriptor& surface) override {
        probe_->surfaces.push_back(surface);
        probe_->surface_threads.push_back(std::this_thread::get_id());
        return probe_->surface_status;
    }

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

        if (probe_->block_first_present && probe_->present_count == 1) {
            std::unique_lock lock(probe_->present_gate_mutex);
            probe_->first_present_entered = true;
            probe_->present_gate_changed.notify_all();
            probe_->present_gate_changed.wait(lock, [this] {
                return probe_->release_first_present;
            });
        }

        if (probe_->device_always_lost ||
            (probe_->device_lost_once && probe_->present_count == 1)) {
            return lapis::media::VideoPresentStatus::DeviceLost;
        }
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

bool wait_for_first_present(const std::shared_ptr<PresenterProbe>& probe) {
    using namespace std::chrono_literals;
    std::unique_lock lock(probe->present_gate_mutex);
    return probe->present_gate_changed.wait_for(
        lock,
        3s,
        [&probe] { return probe->first_present_entered; });
}

void release_first_present(const std::shared_ptr<PresenterProbe>& probe) {
    {
        std::scoped_lock lock(probe->present_gate_mutex);
        probe->release_first_present = true;
    }
    probe->present_gate_changed.notify_all();
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
    CHECK(render_stats.recovery_attempts == 0);
    CHECK(render_stats.recovery_bind_successes == 0);
    CHECK(render_stats.recovered_presents == 0);
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
    CHECK(probe->surfaces.empty());

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
    CHECK(render_stats.recovery_attempts == 0); // No bound surface to recover.
    CHECK(render_stats.recovered_presents == 0);
    CHECK(renderer.last_present_status() == VideoPresentStatus::DeviceLost);
    CHECK(probe->begin_count == 1);
    CHECK(probe->present_count == 1);
    CHECK(probe->end_count == 1);
    CHECK(probe->begin_thread == probe->present_threads[0]);
    CHECK(probe->end_thread == probe->begin_thread);

    // Recover from a transient device removal without restarting playback or
    // replacing the logical epoch. The next decoded frame is presented after
    // the presenter releases and rebuilds its render-thread resources.
    CHECK(media.seek(MediaTime{}));
    const auto recovery_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{11}, MediaTime{}, recovery_now));
    probe->reset_records();
    probe->present_status = VideoPresentStatus::Presented;
    probe->device_lost_once = true;
    const VideoOutputSurfaceDescriptor recovery_surface{
        VideoOutputSurfaceKind::Win32Window,
        static_cast<std::uintptr_t>(0x3333)
    };
    CHECK(renderer.set_output_surface(recovery_surface) ==
        VideoOutputSurfaceControlStatus::Deferred);
    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));
    auto recovery_decoded = decoder.start(PlaybackEpoch{11});
    auto recovery_presentation = scheduler.start(recovery_decoded);
    renderer.start(recovery_presentation);
    CHECK(wait_for_render_completion(renderer));
    renderer.stop();
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    CHECK(renderer.state() == VideoRenderWorkerState::Drained);
    render_stats = renderer.stats();
    CHECK(render_stats.presented_frames == 1);
    CHECK(render_stats.presenter_failures == 1);
    CHECK(render_stats.recovery_attempts == 1);
    CHECK(render_stats.recovery_bind_successes == 1);
    CHECK(render_stats.recovered_presents == 1);
    CHECK(renderer.last_present_status() == VideoPresentStatus::Presented);
    CHECK(probe->begin_count == 2);
    CHECK(probe->present_count == 2);
    CHECK(probe->end_count == 2);
    CHECK(probe->surfaces.size() == 3);
    CHECK(probe->surfaces[0] == recovery_surface);
    CHECK(probe->surfaces[1] == recovery_surface);
    CHECK(probe->surfaces[2] == VideoOutputSurfaceDescriptor{});

    // A repeated loss is NOT a successful EOF merely because the input ran
    // out while the presenter was trying to recreate its D3D resources.
    CHECK(media.seek(MediaTime{}));
    const auto loss_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{12}, MediaTime{}, loss_now));
    probe->reset_records();
    probe->device_always_lost = true;
    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));
    auto loss_decoded = decoder.start(PlaybackEpoch{12});
    auto loss_presentation = scheduler.start(loss_decoded);
    renderer.start(loss_presentation);
    CHECK(wait_for_render_completion(renderer));
    renderer.stop();
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    CHECK(renderer.state() == VideoRenderWorkerState::PresenterUnavailable);
    render_stats = renderer.stats();
    CHECK(render_stats.presented_frames == 0);
    CHECK(render_stats.presenter_failures == 2);
    CHECK(render_stats.recovery_attempts == 2);
    CHECK(render_stats.recovery_bind_successes == 2);
    CHECK(render_stats.recovered_presents == 0);
    CHECK(probe->begin_count == 2);
    CHECK(probe->present_count == 2);
    CHECK(probe->end_count == 2);

    // A surface configured while no render generation exists is deferred. The
    // next generation applies it on the render thread before presentation. A
    // live replacement is synchronous: the caller does not receive Applied until
    // the render thread has left the current present() call and switched surfaces.
    CHECK(media.seek(MediaTime{}));
    const auto surface_now = SteadyClockSource::now();
    CHECK(clock.rebase(PlaybackEpoch{13}, MediaTime{}, surface_now));
    probe->reset_records();
    probe->present_status = VideoPresentStatus::Presented;
    probe->surface_status = VideoOutputSurfaceApplyStatus::Ready;
    probe->block_first_present = true;

    const VideoOutputSurfaceDescriptor first_surface{
        VideoOutputSurfaceKind::Win32Window,
        static_cast<std::uintptr_t>(0x1111)
    };
    const VideoOutputSurfaceDescriptor second_surface{
        VideoOutputSurfaceKind::Win32Window,
        static_cast<std::uintptr_t>(0x2222)
    };
    CHECK(
        renderer.set_output_surface(first_surface) ==
        VideoOutputSurfaceControlStatus::Deferred);

    CHECK(media.start_decode_generation(MediaDecodeSelection{true, false}));
    auto surface_decoded = decoder.start(PlaybackEpoch{13});
    auto surface_presentation = scheduler.start(surface_decoded);
    renderer.start(surface_presentation);
    CHECK(wait_for_first_present(probe));

    std::atomic<VideoOutputSurfaceControlStatus> replacement_status{
        VideoOutputSurfaceControlStatus::Failed
    };
    std::jthread replacement([&] {
        replacement_status.store(
            renderer.set_output_surface(second_surface),
            std::memory_order_release);
    });

    release_first_present(probe);
    replacement.join();
    CHECK(
        replacement_status.load(std::memory_order_acquire) ==
        VideoOutputSurfaceControlStatus::Applied);

    CHECK(wait_for_render_completion(renderer));
    renderer.stop();
    scheduler.stop();
    decoder.stop();
    media.stop_decode_generation();

    CHECK(renderer.state() == VideoRenderWorkerState::Drained);
    CHECK(
        renderer.last_output_surface_apply_status() ==
        VideoOutputSurfaceApplyStatus::Ready);
    CHECK(probe->surfaces.size() == 3);
    CHECK(probe->surfaces[0] == first_surface);
    CHECK(probe->surfaces[1] == second_surface);
    CHECK(probe->surfaces[2] == VideoOutputSurfaceDescriptor{});
    CHECK(probe->surface_threads.size() == 3);
    CHECK(probe->surface_threads[0] == probe->begin_thread);
    CHECK(probe->surface_threads[1] == probe->begin_thread);
    CHECK(probe->surface_threads[2] == probe->begin_thread);
    CHECK(probe->surface_threads[0] != main_thread);

    CHECK(
        renderer.set_output_surface(
            VideoOutputSurfaceDescriptor{VideoOutputSurfaceKind::None, 1}) ==
        VideoOutputSurfaceControlStatus::Invalid);

    return 0;
}
