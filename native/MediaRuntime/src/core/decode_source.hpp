#pragma once
#include "core/decoded_media.hpp"
#include <cstdint>
#include <string>

namespace bdmv::core {
// Owned, blocking, sequential decoder contract. Only the pump's worker thread
// may call next(); seek() is serialized against next() by DecodePump.
// No third-party codec handles/types escape an adapter implementation.
class IMediaDecodeSource {
public:
    virtual ~IMediaDecodeSource() = default;
    [[nodiscard]] virtual bool has_video() const noexcept = 0;
    [[nodiscard]] virtual bool has_audio() const noexcept = 0;
    virtual DecodeReadStatus next(DecodedBuffer& output, std::string& error) = 0;
    virtual bool seek(std::int64_t position_ns, std::string& error) = 0;
};
} // namespace bdmv::core
