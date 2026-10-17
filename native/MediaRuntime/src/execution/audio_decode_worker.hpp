#pragma once

#include "audio/audio_frame.hpp"
#include "core/bounded_queue.hpp"
#include "core/playback_epoch.hpp"
#include "core/timed_payload.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace lapis::media {

class PlaybackMediaSession;
class AudioDecodeWorker;
class AudioProcessWorker;

using TimedAudioFrame = TimedPayload<AudioFrame>;

class AudioFrameStream final {
public:
    [[nodiscard]] PlaybackEpoch epoch() const noexcept {
        return epoch_;
    }

    [[nodiscard]] QueueReadResult<TimedAudioFrame> pop(
        std::stop_token stop_token = {}) {
        return queue_->pop(stop_token);
    }

    [[nodiscard]] QueueReadResult<TimedAudioFrame> try_pop() {
        return queue_->try_pop();
    }

    [[nodiscard]] std::size_t size() const {
        return queue_->size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return queue_->capacity();
    }

private:
    friend class AudioDecodeWorker;
    friend class AudioProcessWorker;

    AudioFrameStream(
        PlaybackEpoch epoch,
        std::shared_ptr<BoundedQueue<TimedAudioFrame>> queue)
        : epoch_(epoch), queue_(std::move(queue)) {}

    PlaybackEpoch epoch_;
    std::shared_ptr<BoundedQueue<TimedAudioFrame>> queue_;
};

enum class AudioDecodeWorkerState : std::uint8_t {
    Idle,
    Running,
    Drained,
    Stopped,
    Faulted
};

class AudioDecodeWorker final {
public:
    explicit AudioDecodeWorker(
        PlaybackMediaSession& media,
        std::size_t queue_capacity = 16);
    ~AudioDecodeWorker() = default;

    AudioDecodeWorker(const AudioDecodeWorker&) = delete;
    AudioDecodeWorker& operator=(const AudioDecodeWorker&) = delete;

    [[nodiscard]] std::shared_ptr<AudioFrameStream> start(PlaybackEpoch epoch);
    void stop();

    [[nodiscard]] AudioDecodeWorkerState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    void run(
        std::stop_token stop_token,
        const std::shared_ptr<AudioFrameStream>& stream) noexcept;

    PlaybackMediaSession& media_;
    const std::size_t queue_capacity_;
    std::atomic<AudioDecodeWorkerState> state_{AudioDecodeWorkerState::Idle};
    mutable std::mutex control_mutex_;
    std::jthread worker_;
};

} // namespace lapis::media
