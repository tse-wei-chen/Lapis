#include "audio/audio_frame.hpp"
#include "decode/ffmpeg/ffmpeg_audio_decoder.hpp"
#include "demux/ffmpeg/ffmpeg_demuxer.hpp"
#include "demux/ffmpeg/ffmpeg_packet_router.hpp"
#include "source/file/file_source.hpp"

#include <cstdint>
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
    using namespace lapis::media::ffmpeg;

    FileSourceProvider provider;
    const MediaLocator locator{MediaLocatorKind::File, LAPIS_TEST_AUDIO_PATH};
    auto source = provider.open(locator);
    CHECK(source.ok());

    FfmpegDemuxer demuxer;
    CHECK(demuxer.open(*source.session->stream, source.session->demux_hint));
    CHECK(!demuxer.best_video_stream().has_value());

    const auto stream = demuxer.best_audio_stream();
    CHECK(stream.has_value());
    CHECK(stream->index >= 0);
    CHECK(stream->sample_rate == 48'000);
    CHECK(stream->channel_count == 2);

    FfmpegAudioDecoder decoder;
    CHECK(decoder.open(*stream));

    FfmpegPacketRouter router(demuxer, {stream->index}, 2);
    router.start();

    std::uint64_t total_sample_frames{};
    std::int64_t total_duration_ns{};
    bool saw_frame{};
    for (;;) {
        auto frame = decoder.decode_next(router);
        if (!frame.has_value()) {
            break;
        }

        saw_frame = true;
        CHECK(frame->buffer != nullptr);
        const auto& format = frame->buffer->format();
        CHECK(format.sample_format == AudioSampleFormat::S16);
        CHECK(format.packing == AudioPacking::Interleaved);
        CHECK(format.sample_rate == 48'000);
        CHECK(format.channels.size() == 2);
        CHECK(format.channel_layout_inferred);
        CHECK(format.channels[0] == AudioChannel::FrontLeft);
        CHECK(format.channels[1] == AudioChannel::FrontRight);
        CHECK(frame->buffer->bytes_per_sample() == 2);
        CHECK(frame->buffer->plane_count() == 1);

        const auto plane = frame->buffer->plane(0);
        CHECK(plane.data != nullptr);
        CHECK(plane.size_bytes ==
            static_cast<std::size_t>(frame->buffer->sample_frame_count()) * 4U);

        total_sample_frames += frame->buffer->sample_frame_count();
        total_duration_ns += frame->duration.nanoseconds;
    }

    router.stop();
    CHECK(saw_frame);
    CHECK(total_sample_frames == 480);
    CHECK(total_duration_ns >= 9'999'998);
    CHECK(total_duration_ns <= 10'000'002);

    return 0;
}
