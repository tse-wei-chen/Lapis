#pragma once

#include "audio/audio_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace lapis::media {

// Checks the decoded-buffer boundary before an output adapter reads any plane.
// This is a structural check, not a device-format negotiation or resampler.
[[nodiscard]] inline bool valid_audio_buffer(const IAudioBuffer& buffer) noexcept {
    const auto& format = buffer.format();
    const auto channel_count = format.channels.size();
    const auto frames = static_cast<std::size_t>(buffer.sample_frame_count());
    const auto bytes = static_cast<std::size_t>(buffer.bytes_per_sample());

    if (format.sample_format == AudioSampleFormat::Unknown ||
        format.sample_rate == 0 || channel_count == 0 || frames == 0 || bytes == 0) {
        return false;
    }

    std::size_t expected_bytes_per_sample{};
    switch (format.sample_format) {
        case AudioSampleFormat::U8: expected_bytes_per_sample = 1; break;
        case AudioSampleFormat::S16: expected_bytes_per_sample = 2; break;
        case AudioSampleFormat::S32:
        case AudioSampleFormat::F32: expected_bytes_per_sample = 4; break;
        case AudioSampleFormat::S64:
        case AudioSampleFormat::F64: expected_bytes_per_sample = 8; break;
        default: return false;
    }
    if (bytes != expected_bytes_per_sample ||
        frames > std::numeric_limits<std::size_t>::max() / bytes) {
        return false;
    }

    const auto per_channel_bytes = frames * bytes;
    const bool planar = format.packing == AudioPacking::Planar;
    if (format.packing != AudioPacking::Interleaved && !planar) {
        return false;
    }
    if (!planar &&
        channel_count > std::numeric_limits<std::size_t>::max() / per_channel_bytes) {
        return false;
    }
    const auto expected_planes = planar ? channel_count : std::size_t{1};
    if (expected_planes > std::numeric_limits<std::uint32_t>::max() ||
        buffer.plane_count() != expected_planes) {
        return false;
    }
    const auto required_bytes = planar
        ? per_channel_bytes
        : per_channel_bytes * channel_count;

    for (std::size_t index = 0; index < expected_planes; ++index) {
        const auto plane = buffer.plane(static_cast<std::uint32_t>(index));
        if (plane.data == nullptr || plane.size_bytes < required_bytes) {
            return false;
        }
    }
    return true;
}

} // namespace lapis::media
