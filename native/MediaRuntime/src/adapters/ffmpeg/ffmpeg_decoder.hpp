#pragma once
#include "core/decode_source.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace bdmv::adapters {
// OPTIONAL real, in-process libavformat/libavcodec software decoder.
// No FFmpeg headers or types cross into portable core or public C ABI.
// This is a sequential DEMUX+DECODE diagnostic backend; scheduling / sinks are M1c.
class FfmpegDecoder final : public core::IMediaDecodeSource {
public:
    static std::unique_ptr<FfmpegDecoder> open_file(const std::string& utf8_path,
                                                    std::string& error);
    ~FfmpegDecoder() override;
    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    [[nodiscard]] bool has_video() const noexcept override;
    [[nodiscard]] bool has_audio() const noexcept override;
    // Exactly one of frame / end / error is returned; EOF flushes delayed frames.
    core::DecodeReadStatus next(core::DecodedBuffer& frame, std::string& error) override;
    // Seek in container timeline; caller must increase epoch and discard older events.
    bool seek(std::int64_t position_ns, std::string& error) override;
private:
    FfmpegDecoder();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bdmv::adapters
