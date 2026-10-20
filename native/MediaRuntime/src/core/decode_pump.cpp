#include "core/decode_pump.hpp"
#include <exception>
#include <stdexcept>
#include <utility>

namespace bdmv::core {
DecodePump::DecodePump(std::unique_ptr<IMediaDecodeSource> decoder,
                       std::uint64_t session_id, std::uint64_t epoch,
                       std::size_t capacity)
    : decoder_(std::move(decoder)), session_id_(session_id), capacity_(capacity),
      has_video_(decoder_ && decoder_->has_video()),
      has_audio_(decoder_ && decoder_->has_audio()), epoch_(epoch) {
    if (!decoder_ || capacity_ == 0) throw std::invalid_argument("invalid decoder pump");
    worker_ = std::thread([this] { run(); });
}
DecodePump::~DecodePump() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        playing_ = false;
        frames_.clear();
        queued_bytes_ = 0;
    }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void DecodePump::set_playing(bool enabled) noexcept {
    {
        std::lock_guard lock(mutex_);
        playing_ = enabled;
    }
    changed_.notify_all();
}
bool DecodePump::seek(std::int64_t position_ns, std::uint64_t new_epoch, std::string& error) {
    if (position_ns < 0) { error = "negative seek"; return false; }
    bool was_playing = false;
    {
        std::lock_guard lock(mutex_);
        was_playing = playing_;
        playing_ = false;
        seeking_ = true;
        epoch_ = new_epoch;
        ended_ = false;
        failed_ = false;
        error_.clear();
        frames_.clear();
        queued_bytes_ = 0;
    }
    changed_.notify_all();
    bool ok = false;
    try {
        std::lock_guard decoder_lock(decoder_mutex_);
        ok = decoder_->seek(position_ns, error);
    } catch (const std::exception& e) { error = e.what(); }
      catch (...) { error = "seek exception"; }
    {
        std::lock_guard lock(mutex_);
        seeking_ = false;
        failed_ = !ok;
        if (!ok) error_ = error;
        playing_ = ok && was_playing;
    }
    changed_.notify_all();
    return ok;
}
DecodePump::ReadStatus DecodePump::status() const noexcept {
    std::lock_guard lock(mutex_);
    if (!frames_.empty()) return ReadStatus::Frame;
    if (failed_) return ReadStatus::Error;
    if (ended_) return ReadStatus::End;
    return ReadStatus::Again;
}
std::string DecodePump::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}
DecodePump::ReadStatus DecodePump::try_read(DecodedBuffer& output) {
    {
        std::lock_guard lock(mutex_);
        if (!frames_.empty()) {
            output = std::move(frames_.front());
            queued_bytes_ -= output.bytes.size();
            frames_.pop_front();
            changed_.notify_all();
            return ReadStatus::Frame;
        }
        if (failed_) return ReadStatus::Error;
        if (ended_) return ReadStatus::End;
    }
    return ReadStatus::Again;
}
void DecodePump::run() noexcept {
    for (;;) {
        std::uint64_t generation = 0;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] {
                return stop_ || (playing_ && !seeking_ && !ended_ && !failed_ && frames_.size() < capacity_);
            });
            if (stop_) return;
            generation = epoch_;
        }
        DecodedBuffer frame;
        std::string error;
        DecodeReadStatus result = DecodeReadStatus::Error;
        try {
            std::lock_guard decoder_lock(decoder_mutex_);
            {
                std::lock_guard lock(mutex_);
                // Protect against a seek that already fenced this generation.
                if (stop_) return;
                if (seeking_ || generation != epoch_ || !playing_) continue;
            }
            result = decoder_->next(frame, error);
        } catch (const std::exception& e) { error = e.what(); }
          catch (...) { error = "decode exception"; }
        {
            std::unique_lock lock(mutex_);
            if (stop_) return;
            if (generation != epoch_ || seeking_) continue; // stale decode result
            if (result == DecodeReadStatus::Frame) {
                const auto bytes = frame.bytes.size();
                if (bytes > kMaxQueueBytes) {
                    failed_ = true;
                    error_ = "decoded frame exceeds CPU buffer limit";
                } else {
                    // A frame can be decoded while the queue is filling. Hold it
                    // until space is available; never silently drop A/V samples.
                    changed_.wait(lock, [this, generation, bytes] {
                        return stop_ || seeking_ || generation != epoch_ ||
                            (frames_.size() < capacity_ && bytes <= kMaxQueueBytes - queued_bytes_);
                    });
                    if (stop_) return;
                    if (seeking_ || generation != epoch_) continue;
                    frame.session_id = session_id_;
                    frame.epoch = generation;
                    try {
                        frames_.push_back(std::move(frame));
                        queued_bytes_ += bytes;
                    } catch (...) {
                        failed_ = true;
                        error_ = "cannot queue decoded frame";
                    }
                }
            } else if (result == DecodeReadStatus::EndOfStream) {
                ended_ = true;
            } else {
                failed_ = true;
                error_ = error.empty() ? "decode failed" : std::move(error);
            }
        }
        changed_.notify_all();
    }
}
} // namespace bdmv::core
