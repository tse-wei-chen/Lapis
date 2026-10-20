#pragma once
#include <bdmv/player_c_api.h>
#include "core/decode_pump.hpp"
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bdmv::core {

// Core domain knows no graphics, audio, FFmpeg, Blu-ray or OS types.
class Session final {
public:
    explicit Session(std::uint64_t id) noexcept : session_id_(id) {}
    bdmv_status open(std::string_view uri) noexcept;
    bdmv_status play() noexcept;
    bdmv_status pause() noexcept;
    bdmv_status seek(std::int64_t position_ns) noexcept;
    bdmv_status close() noexcept;
    bdmv_session_snapshot snapshot() const noexcept;
    bdmv_status read_frame(bdmv_decoded_frame_info* out, std::uint8_t* buffer,
                           std::uint64_t capacity) noexcept;
private:
    mutable std::mutex mutex_;
    std::uint64_t session_id_;
    std::uint64_t epoch_{0};
    std::int64_t position_ns_{0};
    bdmv_transport transport_{BDMV_CLOSED};
    std::unique_ptr<DecodePump> pump_;
    std::optional<DecodedBuffer> pending_;
};

} // namespace bdmv::core
