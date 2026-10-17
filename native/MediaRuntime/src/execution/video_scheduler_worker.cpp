#include "execution/video_scheduler_worker.hpp"

#include "core/monotonic_time.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace lapis::media {

VideoSchedulerWorker::VideoSchedulerWorker(
    PlaybackClock& clock,
    VideoSchedulerConfig scheduler_config,
    std::size_t output_capacity)
    : clock_(clock),
      scheduler_(scheduler_config),
      output_capacity_(output_capacity) {
    if (output_capacity_ == 0) {
        throw std::invalid_argument(
            "VideoSchedulerWorker output capacity must be greater than zero.");
    }
}

std::shared_ptr<VideoPresentationStream> VideoSchedulerWorker::start(
    std::shared_ptr<VideoFrameStream> input) {
    if (input == nullptr) {
        throw std::invalid_argument(
            "VideoSchedulerWorker requires an input stream.");
    }

    std::scoped_lock lock(control_mutex_);
    if (state_.load(std::memory_order_acquire) ==
        VideoSchedulerWorkerState::Running) {
        throw std::logic_error("VideoSchedulerWorker is already running.");
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    auto queue = std::make_shared<BoundedQueue<ScheduledVideoFrame>>(
        output_capacity_);
    auto output = std::shared_ptr<VideoPresentationStream>(
        new VideoPresentationStream(input->epoch(), queue));

    released_frames_.store(0, std::memory_order_relaxed);
    stale_epoch_drops_.store(0, std::memory_order_relaxed);
    late_drops_.store(0, std::memory_order_relaxed);
    state_.store(VideoSchedulerWorkerState::Running, std::memory_order_release);

    try {
        worker_ = std::jthread(
            [this, input = std::move(input), output](std::stop_token stop_token) {
                run(stop_token, input, output);
            });
    } catch (...) {
        state_.store(VideoSchedulerWorkerState::Faulted, std::memory_order_release);
        try {
            queue->close();
        } catch (...) {
            // Preserve the thread-start failure as the primary exception.
        }
        throw;
    }

    return output;
}

void VideoSchedulerWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    worker_.join();
}

VideoSchedulingStats VideoSchedulerWorker::stats() const noexcept {
    return {
        released_frames_.load(std::memory_order_relaxed),
        stale_epoch_drops_.load(std::memory_order_relaxed),
        late_drops_.load(std::memory_order_relaxed)
    };
}

void VideoSchedulerWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<VideoFrameStream>& input,
    const std::shared_ptr<VideoPresentationStream>& output) noexcept {
    auto final_state = VideoSchedulerWorkerState::Stopped;

    try {
        bool should_stop = false;
        while (!stop_token.stop_requested() && !should_stop) {
            auto read_result = input->pop(stop_token);
            if (read_result.status == QueueReadStatus::Stopped) {
                break;
            }
            if (read_result.status == QueueReadStatus::Closed) {
                final_state = VideoSchedulerWorkerState::Drained;
                break;
            }
            if (!read_result.has_item()) {
                continue;
            }

            auto frame = std::move(*read_result.item);
            bool handled = false;
            while (!handled && !stop_token.stop_requested()) {
                const auto clock_snapshot = clock_.snapshot(
                    SteadyClockSource::now());

                if (clock_snapshot.state != PlaybackClockState::Running) {
                    const auto wait_status = clock_.wait_for_change(
                        clock_snapshot.revision,
                        stop_token);
                    if (wait_status == PlaybackClockWaitStatus::Stopped) {
                        should_stop = true;
                    }
                    continue;
                }

                const auto decision = scheduler_.decide(
                    frame.epoch,
                    frame.timestamp,
                    clock_snapshot.epoch,
                    clock_snapshot.position);

                if (decision.action == VideoScheduleAction::Drop) {
                    if (decision.drop_reason == VideoDropReason::StaleEpoch) {
                        stale_epoch_drops_.fetch_add(1, std::memory_order_relaxed);
                    } else if (decision.drop_reason == VideoDropReason::TooLate) {
                        late_drops_.fetch_add(1, std::memory_order_relaxed);
                    }
                    handled = true;
                    continue;
                }

                if (decision.action == VideoScheduleAction::Wait) {
                    const auto wait_status = clock_.wait_for_change_for(
                        clock_snapshot.revision,
                        std::chrono::nanoseconds{
                            decision.wait_duration.nanoseconds
                        },
                        stop_token);
                    if (wait_status == PlaybackClockWaitStatus::Stopped) {
                        should_stop = true;
                    }
                    continue;
                }

                ScheduledVideoFrame scheduled{
                    std::move(frame),
                    clock_snapshot.position,
                    decision.frame_offset
                };
                const auto write_status = output->queue_->push(
                    std::move(scheduled),
                    stop_token);
                if (write_status == QueueWriteStatus::Pushed) {
                    released_frames_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    should_stop = true;
                }
                handled = true;
            }
        }
    } catch (...) {
        final_state = VideoSchedulerWorkerState::Faulted;
    }

    try {
        output->queue_->close();
    } catch (...) {
        final_state = VideoSchedulerWorkerState::Faulted;
    }
    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
