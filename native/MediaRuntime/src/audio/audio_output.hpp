#pragma once

#include "audio/audio_frame.hpp"
#include "core/media_time.hpp"

#include <cstdint>

namespace lapis::media {

// Device-side state is scoped to one playback generation. No FFmpeg, WASAPI,
// or OS-specific handles cross this interface.
struct AudioOutputPosition final {
    // Estimated presentation time of the next sample reaching the speaker.
    // Not equivalent to the timestamp of the last buffer submitted.
    MediaTime presented_until{};
    // Monotonic revision to invalidate cached positions on reset/seek.
    std::uint64_t revision{};
};

enum class AudioOutputStatus : std::uint8_t {
    Accepted,
    Backpressure,
    UnsupportedFormat,
    DeviceLost,
    Faulted
};

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    // Format negotiation is explicit; adapters must not silently reinterpret
    // planar/interleaved layouts or sample rates.
    [[nodiscard]] virtual bool open(const AudioFormat& format) = 0;
    // Non-blocking submission. Caller retains ownership on non-Accepted results.
    // On Accepted, the implementation must retain any sample memory it needs.
    [[nodiscard]] virtual AudioOutputStatus try_submit(const AudioFrame& frame) = 0;
    // Called for discontinuities; invalidates buffered samples and clock revision.
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual AudioOutputPosition position() const noexcept = 0;
    virtual void close() noexcept = 0;
};

} // namespace lapis::media
