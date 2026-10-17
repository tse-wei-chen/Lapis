#include "demux/ffmpeg/ffmpeg_io.hpp"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <cerrno>
#include <cstddef>
#include <limits>
#include <span>

namespace lapis::media::ffmpeg {
namespace {

constexpr int IoBufferSize = 32 * 1024;

int stream_error_to_av(StreamError error) noexcept {
    switch (error) {
        case StreamError::None:
            return 0;
        case StreamError::Unsupported:
            return AVERROR(ENOSYS);
        case StreamError::InvalidArgument:
            return AVERROR(EINVAL);
        case StreamError::IoFailure:
            return AVERROR(EIO);
    }

    return AVERROR(EIO);
}

} // namespace

FfmpegIoContext::FfmpegIoContext(IMediaByteStream& stream)
    : stream_(stream) {
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(IoBufferSize));
    if (buffer == nullptr) {
        return;
    }

    context_ = avio_alloc_context(
        buffer,
        IoBufferSize,
        0,
        this,
        &FfmpegIoContext::read_packet,
        nullptr,
        &FfmpegIoContext::seek);

    if (context_ == nullptr) {
        av_free(buffer);
    }
}

FfmpegIoContext::~FfmpegIoContext() {
    if (context_ != nullptr) {
        avio_context_free(&context_);
    }
}

bool FfmpegIoContext::valid() const noexcept {
    return context_ != nullptr;
}

AVIOContext* FfmpegIoContext::get() const noexcept {
    return context_;
}

int FfmpegIoContext::read_packet(void* opaque, std::uint8_t* buffer, int buffer_size) {
    if (opaque == nullptr || buffer == nullptr || buffer_size <= 0) {
        return AVERROR(EINVAL);
    }

    auto& self = *static_cast<FfmpegIoContext*>(opaque);
    auto bytes = std::span<std::byte>(
        reinterpret_cast<std::byte*>(buffer),
        static_cast<std::size_t>(buffer_size));
    const auto result = self.stream_.read(bytes);

    if (!result.ok()) {
        return stream_error_to_av(result.error);
    }

    if (result.bytes_read == 0 && result.end_of_stream) {
        return AVERROR_EOF;
    }

    return static_cast<int>(result.bytes_read);
}

std::int64_t FfmpegIoContext::seek(void* opaque, std::int64_t offset, int whence) {
    if (opaque == nullptr) {
        return AVERROR(EINVAL);
    }

    auto& self = *static_cast<FfmpegIoContext*>(opaque);

    if ((whence & AVSEEK_SIZE) != 0) {
        const auto stream_size = self.stream_.size();
        if (!stream_size.has_value()) {
            return AVERROR(ENOSYS);
        }
        if (*stream_size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return AVERROR(EOVERFLOW);
        }
        return static_cast<std::int64_t>(*stream_size);
    }

    if (!self.stream_.seekable()) {
        return AVERROR(ENOSYS);
    }

    const auto normalized_whence = whence & ~AVSEEK_FORCE;
    SeekOrigin origin{};
    switch (normalized_whence) {
        case SEEK_SET:
            origin = SeekOrigin::Begin;
            break;
        case SEEK_CUR:
            origin = SeekOrigin::Current;
            break;
        case SEEK_END:
            origin = SeekOrigin::End;
            break;
        default:
            return AVERROR(EINVAL);
    }

    const auto result = self.stream_.seek(offset, origin);
    if (!result.ok()) {
        return stream_error_to_av(result.error);
    }
    if (result.position > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return AVERROR(EOVERFLOW);
    }

    return static_cast<std::int64_t>(result.position);
}

} // namespace lapis::media::ffmpeg
