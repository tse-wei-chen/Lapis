#include "execution/video_render_worker.hpp"

#include "core/monotonic_time.hpp"

#include <stdexcept>
#include <utility>

namespace lapis::media {

VideoRenderWorker::VideoRenderWorker(
    PlaybackClock& clock,
    std::unique_ptr<IVideoPresenter> presenter)
    : clock_(clock), presenter_(std::move(presenter)) {
    if (presenter_ == nullptr) {
        throw std::invalid_argument(
            "VideoRenderWorker requires a presenter.");
    }
}

void VideoRenderWorker::start(
    std::shared_ptr<VideoPresentationStream> input) {
    if (input == nullptr) {
        throw std::invalid_argument(
            "VideoRenderWorker requires an input stream.");
    }

    std::scoped_lock lock(control_mutex_);
    if (state_.load(std::memory_order_acquire) ==
        VideoRenderWorkerState::Running) {
        throw std::logic_error("VideoRenderWorker is already running.");
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    presented_frames_.store(0, std::memory_order_relaxed);
    stale_epoch_drops_.store(0, std::memory_order_relaxed);
    inactive_clock_drops_.store(0, std::memory_order_relaxed);
    presenter_failures_.store(0, std::memory_order_relaxed);
    last_begin_status_.store(
        VideoPresenterBeginStatus::Ready,
        std::memory_order_release);
    last_present_status_.store(
        VideoPresentStatus::Presented,
        std::memory_order_release);
    state_.store(VideoRenderWorkerState::Running, std::memory_order_release);

    try {
        worker_ = std::jthread(
            [this, input = std::move(input)](std::stop_token stop_token) {
                run(stop_token, input);
            });
    } catch (...) {
        state_.store(VideoRenderWorkerState::Faulted, std::memory_order_release);
        throw;
    }
}

void VideoRenderWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    worker_.join();
}

VideoRenderStats VideoRenderWorker::stats() const noexcept {
    return {
        presented_frames_.load(std::memory_order_relaxed),
        stale_epoch_drops_.load(std::memory_order_relaxed),
        inactive_clock_drops_.load(std::memory_order_relaxed),
        presenter_failures_.load(std::memory_order_relaxed)
    };
}

void VideoRenderWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<VideoPresentationStream>& input) noexcept {
    auto final_state = VideoRenderWorkerState::Stopped;
    bool presenter_started = false;

    try {
        while (!stop_token.stop_requested()) {
            auto read_result = input->pop(stop_token);
            if (read_result.status == QueueReadStatus::Stopped) {
                break;
            }
            if (read_result.status == QueueReadStatus::Closed) {
                final_state = VideoRenderWorkerState::Drained;
                break;
            }
            if (!read_result.has_item()) {
                continue;
            }

            const auto& scheduled = *read_result.item;
            const auto clock_snapshot = clock_.snapshot(
                SteadyClockSource::now());

            if (scheduled.frame.epoch.value() != input->epoch().value() ||
                clock_snapshot.epoch.value() != scheduled.frame.epoch.value()) {
                stale_epoch_drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (clock_snapshot.state != PlaybackClockState::Running) {
                inactive_clock_drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (!presenter_started) {
                const auto begin_status = presenter_->begin(input->epoch());
                last_begin_status_.store(begin_status, std::memory_order_release);
                if (begin_status != VideoPresenterBeginStatus::Ready) {
                    presenter_failures_.fetch_add(1, std::memory_order_relaxed);
                    final_state = begin_status == VideoPresenterBeginStatus::Unavailable
                        ? VideoRenderWorkerState::PresenterUnavailable
                        : VideoRenderWorkerState::Faulted;
                    break;
                }
                presenter_started = true;
            }

            const VideoPresentContext context{
                scheduled.frame.epoch,
                scheduled.frame.timestamp,
                scheduled.released_clock_position,
                scheduled.frame_offset,
                clock_snapshot.position
            };

            const auto present_status = presenter_->present(
                scheduled.frame.payload,
                context);
            last_present_status_.store(
                present_status,
                std::memory_order_release);

            if (present_status == VideoPresentStatus::Presented) {
                presented_frames_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            presenter_failures_.fetch_add(1, std::memory_order_relaxed);
            final_state =
                present_status == VideoPresentStatus::UnsupportedSurface ||
                present_status == VideoPresentStatus::DeviceLost
                    ? VideoRenderWorkerState::PresenterUnavailable
                    : VideoRenderWorkerState::Faulted;
            break;
        }
    } catch (...) {
        presenter_failures_.fetch_add(1, std::memory_order_relaxed);
        final_state = VideoRenderWorkerState::Faulted;
    }

    if (presenter_started) {
        presenter_->end();
    }
    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
