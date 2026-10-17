#pragma once

#include "audio/audio_output.hpp"
#include "execution/audio_decode_worker.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

enum class AudioRenderWorkerState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    UnsupportedFormat,
    OutputUnavailable,
    Faulted
};

struct AudioRenderStats final {
    std::uint64_t submitted_frames{};
    std::uint64_t submitted_sample_frames{};
    std::uint64_t stale_epoch_drops{};
    std::uint64_t writable_waits{};
    std::uint64_t output_failures{};
};

class AudioRenderWorker final {
public:
    explicit AudioRenderWorker(std::unique_ptr<IAudioOutput> output);
    ~AudioRenderWorker() = default;

    AudioRenderWorker(const AudioRenderWorker&) = delete;
    AudioRenderWorker& operator=(const AudioRenderWorker&) = delete;

    void start(std::shared_ptr<AudioFrameStream> input);
    void stop();

    [[nodiscard]] AudioRenderWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] AudioRenderStats stats() const noexcept;

    [[nodiscard]] AudioOutputOpenStatus last_open_status() const noexcept {
        return last_open_status_.load(std::memory_order_acquire);
    }

    [[nodiscard]] AudioOutputStatus last_submit_status() const noexcept {
        return last_submit_status_.load(std::memory_order_acquire);
    }

    [[nodiscard]] AudioOutputWaitStatus last_wait_status() const noexcept {
        return last_wait_status_.load(std::memory_order_acquire);
    }

private:
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<AudioFrameStream>& input) noexcept;

    std::unique_ptr<IAudioOutput> output_;
    std::atomic<AudioRenderWorkerState> state_{AudioRenderWorkerState::Idle};
    std::atomic<AudioOutputOpenStatus> last_open_status_{
        AudioOutputOpenStatus::Ready
    };
    std::atomic<AudioOutputStatus> last_submit_status_{
        AudioOutputStatus::Accepted
    };
    std::atomic<AudioOutputWaitStatus> last_wait_status_{
        AudioOutputWaitStatus::Ready
    };
    std::atomic<std::uint64_t> submitted_frames_{};
    std::atomic<std::uint64_t> submitted_sample_frames_{};
    std::atomic<std::uint64_t> stale_epoch_drops_{};
    std::atomic<std::uint64_t> writable_waits_{};
    std::atomic<std::uint64_t> output_failures_{};
    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
