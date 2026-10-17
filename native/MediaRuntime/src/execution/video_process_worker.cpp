#include "execution/video_process_worker.hpp"

#include <stdexcept>
#include <utility>

namespace lapis::media {

VideoProcessWorker::VideoProcessWorker(
    std::unique_ptr<IVideoFrameProcessor> processor,
    std::size_t output_capacity)
    : processor_(std::move(processor)),
      output_capacity_(output_capacity) {
    if (processor_ == nullptr) {
        throw std::invalid_argument(
            "VideoProcessWorker requires a frame processor.");
    }
    if (output_capacity_ == 0) {
        throw std::invalid_argument(
            "VideoProcessWorker output capacity must be greater than zero.");
    }
}

std::shared_ptr<VideoFrameStream> VideoProcessWorker::start(
    std::shared_ptr<VideoFrameStream> input) {
    if (input == nullptr) {
        throw std::invalid_argument(
            "VideoProcessWorker requires an input stream.");
    }

    std::scoped_lock lock(control_mutex_);
    if (state_.load(std::memory_order_acquire) ==
        VideoProcessWorkerState::Running) {
        throw std::logic_error("VideoProcessWorker is already running.");
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    auto queue = std::make_shared<BoundedQueue<TimedVideoFrame>>(
        output_capacity_);
    auto output = std::shared_ptr<VideoFrameStream>(
        new VideoFrameStream(input->epoch(), queue));

    last_status_.store(VideoProcessStatus::Processed, std::memory_order_release);
    state_.store(VideoProcessWorkerState::Running, std::memory_order_release);
    try {
        worker_ = std::jthread(
            [this, input = std::move(input), output](std::stop_token stop_token) {
                run(stop_token, input, output);
            });
    } catch (...) {
        state_.store(VideoProcessWorkerState::Faulted, std::memory_order_release);
        try {
            queue->close();
        } catch (...) {
            // Preserve the thread-start failure as the primary exception.
        }
        throw;
    }

    return output;
}

void VideoProcessWorker::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!worker_.joinable()) {
        return;
    }

    worker_.request_stop();
    worker_.join();
}

void VideoProcessWorker::run(
    std::stop_token stop_token,
    const std::shared_ptr<VideoFrameStream>& input,
    const std::shared_ptr<VideoFrameStream>& output) noexcept {
    auto final_state = VideoProcessWorkerState::Stopped;

    try {
        while (!stop_token.stop_requested()) {
            auto read_result = input->pop(stop_token);
            if (read_result.status == QueueReadStatus::Stopped) {
                break;
            }
            if (read_result.status == QueueReadStatus::Closed) {
                final_state = VideoProcessWorkerState::Drained;
                break;
            }
            if (!read_result.has_item()) {
                continue;
            }

            auto input_frame = std::move(*read_result.item);
            auto process_result = processor_->process(input_frame.payload);
            last_status_.store(process_result.status, std::memory_order_release);

            if (!process_result.ok()) {
                final_state = process_result.status == VideoProcessStatus::UnsupportedInput
                    ? VideoProcessWorkerState::UnsupportedInput
                    : VideoProcessWorkerState::Faulted;
                break;
            }

            auto processed_frame = std::move(*process_result.frame);
            TimedVideoFrame output_frame{
                input_frame.epoch,
                processed_frame.timestamp,
                std::move(processed_frame)
            };

            const auto write_status = output->queue_->push(
                std::move(output_frame),
                stop_token);
            if (write_status != QueueWriteStatus::Pushed) {
                break;
            }
        }
    } catch (...) {
        last_status_.store(VideoProcessStatus::Failed, std::memory_order_release);
        final_state = VideoProcessWorkerState::Faulted;
    }

    try {
        output->queue_->close();
    } catch (...) {
        final_state = VideoProcessWorkerState::Faulted;
    }
    state_.store(final_state, std::memory_order_release);
}

} // namespace lapis::media
