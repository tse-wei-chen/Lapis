#include "audio/wasapi/wasapi_format.hpp"

#if defined(_WIN32)

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace lapis::media::wasapi {
namespace {

constexpr std::array<std::pair<DWORD, AudioChannel>, 18> speaker_channels{{
    {SPEAKER_FRONT_LEFT, AudioChannel::FrontLeft},
    {SPEAKER_FRONT_RIGHT, AudioChannel::FrontRight},
    {SPEAKER_FRONT_CENTER, AudioChannel::FrontCenter},
    {SPEAKER_LOW_FREQUENCY, AudioChannel::LowFrequency},
    {SPEAKER_BACK_LEFT, AudioChannel::BackLeft},
    {SPEAKER_BACK_RIGHT, AudioChannel::BackRight},
    {SPEAKER_FRONT_LEFT_OF_CENTER, AudioChannel::FrontLeftOfCenter},
    {SPEAKER_FRONT_RIGHT_OF_CENTER, AudioChannel::FrontRightOfCenter},
    {SPEAKER_BACK_CENTER, AudioChannel::BackCenter},
    {SPEAKER_SIDE_LEFT, AudioChannel::SideLeft},
    {SPEAKER_SIDE_RIGHT, AudioChannel::SideRight},
    {SPEAKER_TOP_CENTER, AudioChannel::TopCenter},
    {SPEAKER_TOP_FRONT_LEFT, AudioChannel::TopFrontLeft},
    {SPEAKER_TOP_FRONT_CENTER, AudioChannel::TopFrontCenter},
    {SPEAKER_TOP_FRONT_RIGHT, AudioChannel::TopFrontRight},
    {SPEAKER_TOP_BACK_LEFT, AudioChannel::TopBackLeft},
    {SPEAKER_TOP_BACK_CENTER, AudioChannel::TopBackCenter},
    {SPEAKER_TOP_BACK_RIGHT, AudioChannel::TopBackRight}
}};

std::optional<DWORD> speaker_bit(AudioChannel channel) noexcept {
    for (const auto& [bit, mapped_channel] : speaker_channels) {
        if (mapped_channel == channel) {
            return bit;
        }
    }
    return std::nullopt;
}

std::optional<WORD> bits_per_sample(AudioSampleFormat format) noexcept {
    switch (format) {
        case AudioSampleFormat::U8: return 8;
        case AudioSampleFormat::S16: return 16;
        case AudioSampleFormat::S32:
        case AudioSampleFormat::F32:
            return 32;
        case AudioSampleFormat::Unknown:
        case AudioSampleFormat::S64:
        case AudioSampleFormat::F64:
        default:
            return std::nullopt;
    }
}

bool is_float(AudioSampleFormat format) noexcept {
    return format == AudioSampleFormat::F32;
}

std::optional<AudioSampleFormat> parse_sample_format(
    bool floating_point,
    WORD container_bits,
    WORD valid_bits) noexcept {
    if (valid_bits == 0) {
        valid_bits = container_bits;
    }
    if (container_bits == 0 || valid_bits != container_bits) {
        return std::nullopt;
    }

    if (floating_point) {
        return container_bits == 32
            ? std::optional{AudioSampleFormat::F32}
            : std::nullopt;
    }

    switch (container_bits) {
        case 8: return AudioSampleFormat::U8;
        case 16: return AudioSampleFormat::S16;
        case 32: return AudioSampleFormat::S32;
        default: return std::nullopt;
    }
}

std::optional<std::vector<AudioChannel>> parse_channels(
    WORD channel_count,
    DWORD channel_mask,
    bool& inferred) {
    if (channel_count == 0) {
        return std::nullopt;
    }

    inferred = false;
    if (channel_mask == 0) {
        inferred = true;
        if (channel_count == 1) {
            return std::vector<AudioChannel>{AudioChannel::FrontCenter};
        }
        if (channel_count == 2) {
            return std::vector<AudioChannel>{
                AudioChannel::FrontLeft,
                AudioChannel::FrontRight
            };
        }
        return std::nullopt;
    }

    if (std::popcount(channel_mask) != channel_count) {
        return std::nullopt;
    }

    DWORD known_mask{};
    std::vector<AudioChannel> channels;
    channels.reserve(channel_count);
    for (const auto& [bit, channel] : speaker_channels) {
        known_mask |= bit;
        if ((channel_mask & bit) != 0) {
            channels.push_back(channel);
        }
    }

    if ((channel_mask & ~known_mask) != 0 || channels.size() != channel_count) {
        return std::nullopt;
    }
    return channels;
}

} // namespace

std::optional<WAVEFORMATEXTENSIBLE> build_wave_format(
    const AudioFormat& format) noexcept {
    if (format.packing != AudioPacking::Interleaved ||
        format.sample_rate == 0 ||
        format.channels.empty() ||
        format.channels.size() > std::numeric_limits<WORD>::max()) {
        return std::nullopt;
    }

    const auto bits = bits_per_sample(format.sample_format);
    if (!bits.has_value()) {
        return std::nullopt;
    }

    DWORD channel_mask{};
    DWORD previous_bit{};
    for (const auto channel : format.channels) {
        const auto bit = speaker_bit(channel);
        if (!bit.has_value() ||
            (channel_mask & *bit) != 0 ||
            (previous_bit != 0 && *bit <= previous_bit)) {
            return std::nullopt;
        }

        channel_mask |= *bit;
        previous_bit = *bit;
    }

    const auto channel_count = static_cast<std::uint64_t>(format.channels.size());
    const auto bytes_per_sample = static_cast<std::uint64_t>(*bits / 8U);
    const auto block_align = channel_count * bytes_per_sample;
    if (block_align == 0 || block_align > std::numeric_limits<WORD>::max()) {
        return std::nullopt;
    }

    const auto average_bytes_per_second =
        static_cast<std::uint64_t>(format.sample_rate) * block_align;
    if (average_bytes_per_second > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }

    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(format.channels.size());
    wave.Format.nSamplesPerSec = format.sample_rate;
    wave.Format.nAvgBytesPerSec = static_cast<DWORD>(average_bytes_per_second);
    wave.Format.nBlockAlign = static_cast<WORD>(block_align);
    wave.Format.wBitsPerSample = *bits;
    wave.Format.cbSize = static_cast<WORD>(
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    wave.Samples.wValidBitsPerSample = *bits;
    wave.dwChannelMask = channel_mask;
    wave.SubFormat = is_float(format.sample_format)
        ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        : KSDATAFORMAT_SUBTYPE_PCM;
    return wave;
}

std::optional<AudioFormat> parse_wave_format(
    const WAVEFORMATEX& format) {
    if (format.nChannels == 0 ||
        format.nSamplesPerSec == 0 ||
        format.wBitsPerSample == 0 ||
        format.wBitsPerSample % 8 != 0) {
        return std::nullopt;
    }

    bool floating_point{};
    WORD valid_bits = format.wBitsPerSample;
    DWORD channel_mask{};

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        constexpr auto required_extra =
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        if (format.cbSize < required_extra) {
            return std::nullopt;
        }

        const auto& extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            floating_point = false;
        } else if (IsEqualGUID(
                       extensible.SubFormat,
                       KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            floating_point = true;
        } else {
            return std::nullopt;
        }

        valid_bits = extensible.Samples.wValidBitsPerSample;
        channel_mask = extensible.dwChannelMask;
    } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
        floating_point = false;
    } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        floating_point = true;
    } else {
        return std::nullopt;
    }

    const auto sample_format = parse_sample_format(
        floating_point,
        format.wBitsPerSample,
        valid_bits);
    if (!sample_format.has_value()) {
        return std::nullopt;
    }

    const auto bytes_per_sample =
        static_cast<std::uint64_t>(format.wBitsPerSample / 8U);
    const auto expected_block_align =
        static_cast<std::uint64_t>(format.nChannels) * bytes_per_sample;
    if (expected_block_align == 0 ||
        expected_block_align > std::numeric_limits<WORD>::max() ||
        format.nBlockAlign != expected_block_align) {
        return std::nullopt;
    }

    const auto expected_bytes_per_second =
        static_cast<std::uint64_t>(format.nSamplesPerSec) *
        expected_block_align;
    if (expected_bytes_per_second > std::numeric_limits<DWORD>::max() ||
        format.nAvgBytesPerSec != expected_bytes_per_second) {
        return std::nullopt;
    }

    bool inferred{};
    auto channels = parse_channels(
        format.nChannels,
        channel_mask,
        inferred);
    if (!channels.has_value()) {
        return std::nullopt;
    }

    return AudioFormat{
        *sample_format,
        AudioPacking::Interleaved,
        format.nSamplesPerSec,
        std::move(*channels),
        inferred
    };
}

} // namespace lapis::media::wasapi

#endif
