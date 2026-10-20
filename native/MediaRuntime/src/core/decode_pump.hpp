#pragma once
#include "core/decode_source.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bdmv::core {
// Single owned decoder worker, bounded output mailbox and seek generation fence.
// Decode is deliberately NOT real-time paced: output/presentation belongs to a
// future sink/scheduler. Close/seek may block until one local-file decode finishes.
class DecodePump final {
public:
    enum class ReadStatus { Frame, Again, End, Error };
    DecodePump(std::unique_ptr<IMediaDecodeSource> decoder,
               std::uint64_t session_id, std::uint64_t epoch,
               std::size_t capacity = 8);
    ~DecodePump();
    DecodePump(const DecodePump&) = delete;
    DecodePump& operator=(const DecodePump&) = delete;

    void set_playing(bool enabled) noexcept;
    bool seek(std::int64_t position_ns, std::uint64_t new_epoch, std::string& error);
    ReadStatus try_read(DecodedBuffer& output);
    [[nodiscard]] ReadStatus status() const noexcept;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] bool has_video() const noexcept { return has_video_; }
    [[nodiscard]] bool has_audio() const noexcept { return has_audio_; }
private:
    void run() noexcept;
    std::unique_ptr<IMediaDecodeSource> decoder_;
    const std::uint64_t session_id_;
    const std::size_t capacity_;
    const bool has_video_;
    const bool has_audio_;
    mutable std::mutex mutex_;
    std::mutex decoder_mutex_;
    std::condition_variable changed_;
    std::deque<DecodedBuffer> frames_;
    std::size_t queued_bytes_{0};
    static constexpr std::size_t kMaxQueueBytes = 128ULL * 1024 * 1024;
    std::thread worker_;
    std::uint64_t epoch_;
    bool playing_{false};
    bool seeking_{false};
    bool stop_{false};
    bool ended_{false};
    bool failed_{false};
    std::string error_;
};
} // namespace bdmv::core
