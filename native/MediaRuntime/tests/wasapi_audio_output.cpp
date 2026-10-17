#include "audio/wasapi/wasapi_format.hpp"
#include "audio/wasapi/wasapi_shared_audio_output.hpp"
#include "audio/wasapi/wasapi_shared_audio_output_provider.hpp"

#if defined(_WIN32)

#include <iostream>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main() {
    using namespace lapis::media;
    using namespace lapis::media::wasapi;

    const AudioFormat stereo_float{
        AudioSampleFormat::F32,
        AudioPacking::Interleaved,
        48'000,
        {AudioChannel::FrontLeft, AudioChannel::FrontRight},
        false
    };
    const auto stereo = build_wave_format(stereo_float);
    CHECK(stereo.has_value());
    CHECK(stereo->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    CHECK(stereo->Format.nChannels == 2);
    CHECK(stereo->Format.nSamplesPerSec == 48'000);
    CHECK(stereo->Format.nBlockAlign == 8);
    CHECK(stereo->Format.nAvgBytesPerSec == 384'000);
    CHECK(stereo->Format.wBitsPerSample == 32);
    CHECK(stereo->Samples.wValidBitsPerSample == 32);
    CHECK(stereo->dwChannelMask == (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT));
    CHECK(IsEqualGUID(stereo->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));

    const auto parsed_stereo = parse_wave_format(stereo->Format);
    CHECK(parsed_stereo.has_value());
    CHECK(parsed_stereo->sample_format == AudioSampleFormat::F32);
    CHECK(parsed_stereo->packing == AudioPacking::Interleaved);
    CHECK(parsed_stereo->sample_rate == 48'000);
    CHECK(parsed_stereo->channels == stereo_float.channels);
    CHECK(!parsed_stereo->channel_layout_inferred);

    const AudioFormat surround_pcm{
        AudioSampleFormat::S16,
        AudioPacking::Interleaved,
        48'000,
        {
            AudioChannel::FrontLeft,
            AudioChannel::FrontRight,
            AudioChannel::FrontCenter,
            AudioChannel::LowFrequency,
            AudioChannel::BackLeft,
            AudioChannel::BackRight
        },
        false
    };
    const auto surround = build_wave_format(surround_pcm);
    CHECK(surround.has_value());
    CHECK(surround->Format.nChannels == 6);
    CHECK(surround->Format.nBlockAlign == 12);
    CHECK(surround->dwChannelMask == (
        SPEAKER_FRONT_LEFT |
        SPEAKER_FRONT_RIGHT |
        SPEAKER_FRONT_CENTER |
        SPEAKER_LOW_FREQUENCY |
        SPEAKER_BACK_LEFT |
        SPEAKER_BACK_RIGHT));
    CHECK(IsEqualGUID(surround->SubFormat, KSDATAFORMAT_SUBTYPE_PCM));

    const auto parsed_surround = parse_wave_format(surround->Format);
    CHECK(parsed_surround.has_value());
    CHECK(parsed_surround->sample_format == AudioSampleFormat::S16);
    CHECK(parsed_surround->channels == surround_pcm.channels);
    CHECK(!parsed_surround->channel_layout_inferred);

    WAVEFORMATEX legacy_stereo{};
    legacy_stereo.wFormatTag = WAVE_FORMAT_PCM;
    legacy_stereo.nChannels = 2;
    legacy_stereo.nSamplesPerSec = 44'100;
    legacy_stereo.nAvgBytesPerSec = 176'400;
    legacy_stereo.nBlockAlign = 4;
    legacy_stereo.wBitsPerSample = 16;
    const auto parsed_legacy = parse_wave_format(legacy_stereo);
    CHECK(parsed_legacy.has_value());
    CHECK(parsed_legacy->sample_format == AudioSampleFormat::S16);
    CHECK(parsed_legacy->sample_rate == 44'100);
    CHECK(parsed_legacy->channels.size() == 2);
    CHECK(parsed_legacy->channels[0] == AudioChannel::FrontLeft);
    CHECK(parsed_legacy->channels[1] == AudioChannel::FrontRight);
    CHECK(parsed_legacy->channel_layout_inferred);

    AudioFormat planar = stereo_float;
    planar.packing = AudioPacking::Planar;
    CHECK(!build_wave_format(planar).has_value());

    // Construct both concrete adapter types so the endpoint/output translation
    // units are linked. Unsupported input exits before endpoint discovery, which
    // keeps this deterministic on CI runners without an audio device.
    WasapiSharedAudioOutput output;
    WasapiSharedAudioOutputProvider provider;
    (void)provider;
    CHECK(output.position().revision == 0);
    CHECK(output.open(planar) == AudioOutputOpenStatus::UnsupportedFormat);
    CHECK(output.position().revision == 0);
    output.close();

    const AudioFormat reversed_stereo{
        AudioSampleFormat::F32,
        AudioPacking::Interleaved,
        48'000,
        {AudioChannel::FrontRight, AudioChannel::FrontLeft},
        false
    };
    CHECK(!build_wave_format(reversed_stereo).has_value());

    const AudioFormat duplicate_channel{
        AudioSampleFormat::S16,
        AudioPacking::Interleaved,
        48'000,
        {AudioChannel::FrontLeft, AudioChannel::FrontLeft},
        false
    };
    CHECK(!build_wave_format(duplicate_channel).has_value());

    const AudioFormat unknown_channel{
        AudioSampleFormat::S16,
        AudioPacking::Interleaved,
        48'000,
        {AudioChannel::Unknown},
        false
    };
    CHECK(!build_wave_format(unknown_channel).has_value());

    AudioFormat unsupported_sample = stereo_float;
    unsupported_sample.sample_format = AudioSampleFormat::F64;
    CHECK(!build_wave_format(unsupported_sample).has_value());

    AudioFormat missing_rate = stereo_float;
    missing_rate.sample_rate = 0;
    CHECK(!build_wave_format(missing_rate).has_value());

    auto reduced_precision = *surround;
    reduced_precision.Format.wBitsPerSample = 32;
    reduced_precision.Format.nBlockAlign = 24;
    reduced_precision.Format.nAvgBytesPerSec = 1'152'000;
    reduced_precision.Samples.wValidBitsPerSample = 24;
    CHECK(!parse_wave_format(reduced_precision.Format).has_value());

    auto unknown_mask = *surround;
    unknown_mask.dwChannelMask = 0x80000000UL;
    CHECK(!parse_wave_format(unknown_mask.Format).has_value());

    auto malformed_rate = *stereo;
    malformed_rate.Format.nAvgBytesPerSec -= 1;
    CHECK(!parse_wave_format(malformed_rate.Format).has_value());

    return 0;
}

#endif
