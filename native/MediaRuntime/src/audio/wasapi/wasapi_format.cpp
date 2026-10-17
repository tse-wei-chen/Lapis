#include "audio/wasapi/wasapi_format.hpp"

#if defined(_WIN32)

#include <cstdint>
#include <limits>
#include <optional>

namespace lapis::media::wasapi {
namespace {

std::optional<DWORD> speaker_bit(AudioChannel channel) noexcept {
    switch (channel) {
        case AudioChannel::FrontLeft: return SPEAKER_FRONT_LEFT;
        case AudioChannel::FrontRight: return SPEAKER_FRONT_RIGHT;
        case AudioChannel::FrontCenter: return SPEAKER_FRONT_CENTER;
        case AudioChannel::LowFrequency: return SPEAKER_LOW_FREQUENCY;
        case AudioChannel::BackLeft: return SPEAKER_BACK_LEFT;
        case AudioChannel::BackRight: return SPEAKER_BACK_RIGHT;
        case AudioChannel::FrontLeftOfCenter: return SPEAKER_FRONT_LEFT_OF_CENTER;
        case AudioChannel::FrontRightOfCenter: return SPEAKER_FRONT_RIGHT_OF_CENTER;
        case AudioChannel::BackCenter: return SPEAKER_BACK_CENTER;
        case AudioChannel::SideLeft: return SPEAKER_SIDE_LEFT;
        case AudioChannel::SideRight: return SPEAKER_SIDE_RIGHT;
        case AudioChannel::TopCenter: return SPEAKER_TOP_CENTER;
        case AudioChannel::TopFrontLeft: return SPEAKER_TOP_FRONT_LEFT;
        case AudioChannel::TopFrontCenter: return SPEAKER_TOP_FRONT_CENTER;
        case AudioChannel::TopFrontRight: return SPEAKER_TOP_FRONT_RIGHT;
        case AudioChannel::TopBackLeft: return SPEAKER_TOP_BACK_LEFT;
        case AudioChannel::TopBackCenter: return SPEAKER_TOP_BACK_CENTER;
        case AudioChannel::TopBackRight: return SPEAKER_TOP_BACK_RIGHT;
        case AudioChannel::Unknown:
        default:
            return std::nullopt;
    }
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

} // namespace lapis::media::wasapi

#endif
