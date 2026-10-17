#include "audio/wasapi/wasapi_format.hpp"
#include "audio/wasapi/wasapi_shared_audio_output.hpp"

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

    AudioFormat planar = stereo_float;
    planar.packing = AudioPacking::Planar;
    CHECK(!build_wave_format(planar).has_value());

    // Construct the concrete adapter so this test also links its COM/WASAPI
    // implementation. Unsupported input exits before endpoint discovery, keeping
    // the test deterministic on CI runners without an audio device.
    WasapiSharedAudioOutput output;
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

    return 0;
}

#endif
