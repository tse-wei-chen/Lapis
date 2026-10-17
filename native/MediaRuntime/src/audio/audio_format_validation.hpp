#pragma once

#include "audio/audio_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

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

// Returns a byte view for a contiguous sample-frame slice in one plane. Callers
// validate the whole buffer once with valid_audio_buffer() before using this in
// the hot path. Interleaved slices include every channel for each sample frame;
// planar slices include only the selected channel plane.
[[nodiscard]] inline std::optional<AudioPlaneView> audio_plane_slice(
    const IAudioBuffer& buffer,
    std::uint32_t plane_index,
    std::uint32_t first_sample_frame,
    std::uint32_t sample_frame_count) noexcept {
    const auto total_frames = buffer.sample_frame_count();
    if (sample_frame_count == 0 || first_sample_frame >= total_frames ||
        sample_frame_count > total_frames - first_sample_frame) {
        return std::nullopt;
    }

    const auto& format = buffer.format();
    const auto channel_count = format.channels.size();
    const auto bytes_per_sample = static_cast<std::size_t>(buffer.bytes_per_sample());
    if (channel_count == 0 || bytes_per_sample == 0) {
        return std::nullopt;
    }

    std::size_t channel_factor{};
    if (format.packing == AudioPacking::Planar) {
        if (plane_index >= channel_count) {
            return std::nullopt;
        }
        channel_factor = 1;
    } else if (format.packing == AudioPacking::Interleaved) {
        if (plane_index != 0) {
            return std::nullopt;
        }
        channel_factor = channel_count;
    } else {
        return std::nullopt;
    }

    if (channel_factor > std::numeric_limits<std::size_t>::max() / bytes_per_sample) {
        return std::nullopt;
    }
    const auto bytes_per_frame = bytes_per_sample * channel_factor;
    const auto first = static_cast<std::size_t>(first_sample_frame);
    const auto count = static_cast<std::size_t>(sample_frame_count);
    if (first > std::numeric_limits<std::size_t>::max() / bytes_per_frame ||
        count > std::numeric_limits<std::size_t>::max() / bytes_per_frame) {
        return std::nullopt;
    }

    const auto offset_bytes = first * bytes_per_frame;
    const auto slice_bytes = count * bytes_per_frame;
    if (offset_bytes > std::numeric_limits<std::size_t>::max() - slice_bytes) {
        return std::nullopt;
    }

    const auto plane = buffer.plane(plane_index);
    if (plane.data == nullptr || plane.size_bytes < offset_bytes + slice_bytes) {
        return std::nullopt;
    }
    return AudioPlaneView{plane.data + offset_bytes, slice_bytes};
}

} // namespace lapis::media
