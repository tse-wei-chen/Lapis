#include "core/session.hpp"
#ifdef BDMV_ENABLE_TEST_SOURCE
#include "adapters/synthetic_source.hpp"
#endif
#ifdef BDMV_WITH_FFMPEG
#include "adapters/ffmpeg/ffmpeg_decoder.hpp"
#endif
#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace bdmv::core {

bdmv_status Session::open(std::string_view uri) noexcept {
    if (uri.empty()) return BDMV_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    if (transport_ != BDMV_CLOSED) return BDMV_INVALID_STATE;
    try {
        std::unique_ptr<IMediaDecodeSource> decoder;
#ifdef BDMV_ENABLE_TEST_SOURCE
        if (uri == "test://synthetic") {
            transport_ = BDMV_READY;
            position_ns_ = 0;
            ++epoch_;
            return BDMV_OK;
        }
        if (uri == "test://frames") decoder = std::make_unique<adapters::SyntheticSource>();
#endif
#ifdef BDMV_WITH_FFMPEG
        if (!decoder && uri.find("://") == std::string_view::npos) {
            std::string error;
            decoder = adapters::FfmpegDecoder::open_file(std::string(uri), error);
            if (!decoder) return BDMV_UNSUPPORTED;
        } else if (!decoder && uri.starts_with("file:///")) {
            // Accept only simple local file URIs. Percent-escaped paths require
            // explicit URI parsing, not lossy ad-hoc replacement.
            std::string path(uri.substr(7));
            if (path.find('%') != std::string::npos || path.find('?') != std::string::npos ||
                path.find('#') != std::string::npos) return BDMV_INVALID_ARGUMENT;
#ifdef _WIN32
            if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
#endif
            std::string error;
            decoder = adapters::FfmpegDecoder::open_file(path, error);
            if (!decoder) return BDMV_UNSUPPORTED;
        }
#endif
        if (!decoder) return BDMV_UNSUPPORTED;
        const auto next_epoch = epoch_ + 1;
        auto pump = std::make_unique<DecodePump>(std::move(decoder), session_id_, next_epoch);
        pump_ = std::move(pump);
        epoch_ = next_epoch;
        position_ns_ = 0;
        transport_ = BDMV_READY;
        return BDMV_OK;
    } catch (const std::bad_alloc&) { return BDMV_OUT_OF_MEMORY; }
      catch (...) { return BDMV_INTERNAL_ERROR; }
}

bdmv_status Session::play() noexcept {
    std::lock_guard lock(mutex_);
    if (transport_ != BDMV_READY && transport_ != BDMV_PAUSED) return BDMV_INVALID_STATE;
    transport_ = BDMV_PLAYING;
    if (pump_) pump_->set_playing(true);
    return BDMV_OK;
}

bdmv_status Session::pause() noexcept {
    std::lock_guard lock(mutex_);
    if (transport_ != BDMV_PLAYING) return BDMV_INVALID_STATE;
    if (pump_) pump_->set_playing(false);
    transport_ = BDMV_PAUSED;
    return BDMV_OK;
}

bdmv_status Session::seek(std::int64_t position_ns) noexcept {
    if (position_ns < 0) return BDMV_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    if (transport_ != BDMV_READY && transport_ != BDMV_PAUSED &&
        transport_ != BDMV_PLAYING && transport_ != BDMV_ENDED) return BDMV_INVALID_STATE;
    const auto previous = transport_;
    transport_ = BDMV_SEEKING;
    ++epoch_;
    pending_.reset();
    if (pump_) {
        std::string error;
        bool ok = false;
        try { ok = pump_->seek(position_ns, epoch_, error); }
        catch (...) { ok = false; }
        if (!ok) { transport_ = BDMV_FAULTED; return BDMV_DECODE_ERROR; }
    }
    position_ns_ = position_ns;
    transport_ = previous == BDMV_ENDED ? BDMV_PAUSED : previous;
    return BDMV_OK;
}

bdmv_status Session::close() noexcept {
    std::lock_guard lock(mutex_);
    if (transport_ != BDMV_CLOSED) {
        ++epoch_;
        pump_.reset(); // stops and joins decoder worker before destroying codec resources
        pending_.reset();
        transport_ = BDMV_CLOSED;
        position_ns_ = 0;
    }
    return BDMV_OK;
}

bdmv_status Session::read_frame(bdmv_decoded_frame_info* out,
                                std::uint8_t* buffer, std::uint64_t capacity) noexcept {
    if (!out || out->struct_size < sizeof(bdmv_decoded_frame_info)) return BDMV_ABI_MISMATCH;
    std::lock_guard lock(mutex_);
    if (!pump_ || (transport_ != BDMV_READY && transport_ != BDMV_PLAYING &&
                   transport_ != BDMV_PAUSED && transport_ != BDMV_ENDED)) return BDMV_INVALID_STATE;
    if (!pending_) {
        // Move-only buffers avoid copying large decoded video surfaces between threads.
        DecodedBuffer next;
        const auto result = pump_->try_read(next);
        if (result == DecodePump::ReadStatus::Again) return BDMV_AGAIN;
        if (result == DecodePump::ReadStatus::Error) {
            transport_ = BDMV_FAULTED;
            return BDMV_DECODE_ERROR;
        }
        if (result == DecodePump::ReadStatus::End) {
            transport_ = BDMV_ENDED;
            return BDMV_END_OF_STREAM;
        }
        try { pending_.emplace(std::move(next)); }
        catch (...) { return BDMV_OUT_OF_MEMORY; }
    }
    const auto& frame = *pending_;
    if (frame.session_id != session_id_ || frame.epoch != epoch_) {
        pending_.reset();
        return BDMV_AGAIN;
    }
    *out = {sizeof(bdmv_decoded_frame_info),
            static_cast<std::uint32_t>(frame.kind), frame.session_id, frame.epoch,
            frame.pts_ns, frame.duration_ns, frame.width, frame.height,
            frame.sample_rate, frame.channels, static_cast<std::uint64_t>(frame.bytes.size())};
    if (frame.bytes.size() > capacity || (buffer == nullptr && !frame.bytes.empty()))
        return BDMV_BUFFER_TOO_SMALL;
    if (!frame.bytes.empty()) std::memcpy(buffer, frame.bytes.data(), frame.bytes.size());
    if (frame.pts_ns >= 0) position_ns_ = frame.pts_ns;
    pending_.reset();
    return BDMV_OK;
}

bdmv_session_snapshot Session::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    bdmv_transport state = transport_;
    if (state == BDMV_PLAYING && pump_) {
        if (pump_->status() == DecodePump::ReadStatus::Error) state = BDMV_FAULTED;
        else if (!pending_ && pump_->status() == DecodePump::ReadStatus::End) state = BDMV_ENDED;
    }
    return bdmv_session_snapshot{
        static_cast<std::uint32_t>(sizeof(bdmv_session_snapshot)),
        static_cast<std::uint32_t>(state), session_id_, epoch_, position_ns_,
        0, 0, 0, 0
    };
}

} // namespace bdmv::core
