#pragma once

#include "audio/audio_frame.hpp"
#include "core/media_time.hpp"

#include <cstdint>
#include <stop_token>

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

enum class AudioOutputOpenStatus : std::uint8_t {
    Ready,
    UnsupportedFormat,
    DeviceLost,
    Faulted
};

enum class AudioOutputStatus : std::uint8_t {
    Accepted,
    Backpressure,
    UnsupportedFormat,
    DeviceLost,
    Faulted
};

enum class AudioOutputWaitStatus : std::uint8_t {
    Ready,
    Stopped,
    DeviceLost,
    Faulted
};

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    // Format negotiation is explicit; adapters must not silently reinterpret
    // planar/interleaved layouts or sample rates. A failed open must leave the
    // adapter closed and safe to reuse for a later generation.
    [[nodiscard]] virtual AudioOutputOpenStatus open(
        const AudioFormat& format) = 0;

    // Non-blocking submission. Caller retains ownership on non-Accepted results.
    // On Accepted, the implementation must retain any sample memory it needs.
    [[nodiscard]] virtual AudioOutputStatus try_submit(
        const AudioFrame& frame) = 0;

    // A Backpressure result is followed by this stop-aware wait. Concrete
    // adapters should map it to their native readiness primitive (for example,
    // an event-driven audio client) rather than polling.
    [[nodiscard]] virtual AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) = 0;

    // End-of-input is not end-of-presentation. This wait completes only after
    // all accepted samples have left the device queue, or shutdown interrupts it.
    [[nodiscard]] virtual AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) = 0;

    // Called for discontinuities; invalidates buffered samples and clock revision.
    virtual void reset() noexcept = 0;

    // Implementations must make this a thread-safe snapshot. Future clock
    // discipline may query it outside the device submission thread.
    [[nodiscard]] virtual AudioOutputPosition position() const noexcept = 0;

    virtual void close() noexcept = 0;
};

} // namespace lapis::media
