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

struct AudioOutputSubmitResult final {
    AudioOutputStatus status{AudioOutputStatus::Faulted};
    std::uint32_t accepted_sample_frames{};
};

enum class AudioOutputWaitStatus : std::uint8_t {
    Ready,
    Stopped,
    DeviceLost,
    Faulted
};

enum class AudioOutputControlStatus : std::uint8_t {
    Ready,
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

    // Non-blocking prefix submission. first_sample_frame identifies the first
    // not-yet-submitted sample in frame. Accepted must consume a positive prefix
    // no larger than the remaining sample count. If only a partial prefix fits,
    // the adapter reports Accepted for that prefix and the caller waits for
    // writability before retrying the remainder. Backpressure and error results
    // consume zero samples. Implementations must copy or otherwise retain any
    // accepted sample content before returning.
    [[nodiscard]] virtual AudioOutputSubmitResult try_submit(
        const AudioFrame& frame,
        std::uint32_t first_sample_frame) = 0;

    // Backpressure or a partial accepted prefix is followed by this stop-aware
    // wait. The token may represent generation shutdown or a render-thread
    // control interruption; callers decide which after the wait returns Stopped.
    [[nodiscard]] virtual AudioOutputWaitStatus wait_until_writable(
        std::stop_token stop_token) = 0;

    // End-of-input is not end-of-presentation. This wait completes only after
    // all accepted samples have left the device queue, or its wait is interrupted.
    [[nodiscard]] virtual AudioOutputWaitStatus wait_until_drained(
        std::stop_token stop_token) = 0;

    // Pause/resume preserve queued samples and device stream position. These are
    // called by the render worker on the same execution thread that owns stream
    // submission; control-plane callers never invoke device APIs directly.
    [[nodiscard]] virtual AudioOutputControlStatus pause() = 0;
    [[nodiscard]] virtual AudioOutputControlStatus resume() = 0;

    // Called for discontinuities; invalidates buffered samples and clock revision.
    virtual void reset() noexcept = 0;

    // Implementations must make this a thread-safe snapshot. Future clock
    // discipline may query it outside the device submission thread.
    [[nodiscard]] virtual AudioOutputPosition position() const noexcept = 0;

    virtual void close() noexcept = 0;
};

} // namespace lapis::media
