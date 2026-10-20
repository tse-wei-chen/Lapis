#pragma once
#include "core/decode_source.hpp"
#include <cstdint>

namespace bdmv::adapters {
// Diagnostic finite fake stream. Not a video/audio device or a media file.
class SyntheticSource final : public core::IMediaDecodeSource {
public:
    bool has_video() const noexcept override { return true; }
    bool has_audio() const noexcept override { return true; }
    core::DecodeReadStatus next(core::DecodedBuffer& frame, std::string& error) override;
    bool seek(std::int64_t ns, std::string& error) override;
private:
    std::uint32_t cursor_{0};
};
} // namespace bdmv::adapters
