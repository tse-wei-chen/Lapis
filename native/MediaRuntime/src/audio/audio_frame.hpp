#pragma once

#include "core/media_time.hpp"
#include "core/rational.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lapis::media {

enum class AudioSampleFormat : std::uint8_t {
    Unknown,
    U8,
    S16,
    S32,
    S64,
    F32,
    F64
};

enum class AudioPacking : std::uint8_t {
    Interleaved,
    Planar
};

enum class AudioChannel : std::uint8_t {
    Unknown,
    FrontLeft,
    FrontRight,
    FrontCenter,
    LowFrequency,
    BackLeft,
    BackRight,
    FrontLeftOfCenter,
    FrontRightOfCenter,
    BackCenter,
    SideLeft,
    SideRight,
    TopCenter,
    TopFrontLeft,
    TopFrontCenter,
    TopFrontRight,
    TopBackLeft,
    TopBackCenter,
    TopBackRight
};

struct AudioFormat final {
    AudioSampleFormat sample_format{AudioSampleFormat::Unknown};
    AudioPacking packing{AudioPacking::Interleaved};
    std::uint32_t sample_rate{};
    std::vector<AudioChannel> channels;
    bool channel_layout_inferred{};
};

struct AudioPlaneView final {
    const std::byte* data{};
    std::size_t size_bytes{};
};

class IAudioBuffer {
public:
    virtual ~IAudioBuffer() = default;

    [[nodiscard]] virtual const AudioFormat& format() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sample_frame_count() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t bytes_per_sample() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t plane_count() const noexcept = 0;
    [[nodiscard]] virtual AudioPlaneView plane(std::uint32_t index) const noexcept = 0;
};

struct AudioFrame final {
    std::shared_ptr<IAudioBuffer> buffer;
    MediaTime timestamp{};
    MediaTime duration{};
    std::optional<SourceTimestamp> source_timestamp;
};

} // namespace lapis::media
