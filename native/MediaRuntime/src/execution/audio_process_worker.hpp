#pragma once

#include "audio/audio_processor.hpp"
#include "execution/audio_decode_worker.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

enum class AudioProcessWorkerState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    UnsupportedInput,
    Faulted
};

class AudioProcessWorker final {
public:
    AudioProcessWorker(
        std::unique_ptr<IAudioFrameProcessor> processor,
        std::size_t output_capacity = 8);
    ~AudioProcessWorker() = default;

    AudioProcessWorker(const AudioProcessWorker&) = delete;
    AudioProcessWorker& operator=(const AudioProcessWorker&) = delete;

    [[nodiscard]] std::shared_ptr<AudioFrameStream> start(
        std::shared_ptr<AudioFrameStream> input);
    void stop();

    [[nodiscard]] AudioProcessWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] AudioProcessStatus last_status() const noexcept {
        return last_status_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const AudioFormat& output_format() const noexcept {
        return processor_->output_format();
    }

private:
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<AudioFrameStream>& input,
        const std::shared_ptr<AudioFrameStream>& output) noexcept;

    [[nodiscard]] bool publish(
        AudioProcessResult result,
        PlaybackEpoch epoch,
        std::stop_token stop_token,
        const std::shared_ptr<AudioFrameStream>& output);

    std::unique_ptr<IAudioFrameProcessor> processor_;
    const std::size_t output_capacity_;
    std::atomic<AudioProcessWorkerState> state_{AudioProcessWorkerState::Idle};
    std::atomic<AudioProcessStatus> last_status_{AudioProcessStatus::NeedMoreInput};
    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
