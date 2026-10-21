#pragma once

#include "source/media_source.hpp"

#include <atomic>
#include <cstdint>

struct AVIOContext;

namespace lapis::media::ffmpeg {

class FfmpegIoContext final {
public:
    FfmpegIoContext(IMediaByteStream& stream,
                    const std::atomic<bool>& interrupt_requested);
    ~FfmpegIoContext();

    FfmpegIoContext(const FfmpegIoContext&) = delete;
    FfmpegIoContext& operator=(const FfmpegIoContext&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] AVIOContext* get() const noexcept;

private:
    static int read_packet(void* opaque, std::uint8_t* buffer, int buffer_size);
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence);

    IMediaByteStream& stream_;
    const std::atomic<bool>& interrupt_requested_;
    AVIOContext* context_{};
};

} // namespace lapis::media::ffmpeg
