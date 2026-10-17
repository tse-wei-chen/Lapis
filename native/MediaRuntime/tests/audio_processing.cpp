#include "audio/audio_frame.hpp"
#include "execution/audio_decode_worker.hpp"
#include "execution/audio_process_worker.hpp"
#include "process/ffmpeg/ffmpeg_audio_frame_converter.hpp"
#include "session/playback_media_session.hpp"

#include <cstdint>
#include <iostream>
#include <memory>

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

    PlaybackMediaSession media;
    const MediaLocator locator{MediaLocatorKind::File, LAPIS_TEST_AUDIO_PATH};
    const auto opened = media.open(locator);
    CHECK(opened.ok());
    CHECK(opened.has_audio);
    CHECK(media.start_decode_generation({false, true}));

    AudioDecodeWorker decoder(media, 1);
    auto decoded = decoder.start(PlaybackEpoch{31});

    AudioFormat target{
        AudioSampleFormat::F32,
        AudioPacking::Interleaved,
        44'100,
        {AudioChannel::FrontLeft, AudioChannel::FrontRight},
        false
    };
    auto converter = std::make_unique<FfmpegAudioFrameConverter>(target);
    AudioProcessWorker processor(std::move(converter), 1);
    auto processed = processor.start(decoded);

    std::uint64_t total_sample_frames{};
    std::int64_t total_duration_ns{};
    bool saw_output{};
    for (;;) {
        auto result = processed->pop();
        if (result.status == QueueReadStatus::Closed) {
            break;
        }
        CHECK(result.has_item());
        CHECK(result.item->epoch.value() == 31);
        CHECK(result.item->timestamp == result.item->payload.timestamp);
        CHECK(result.item->payload.buffer != nullptr);

        const auto& format = result.item->payload.buffer->format();
        CHECK(format.sample_format == AudioSampleFormat::F32);
        CHECK(format.packing == AudioPacking::Interleaved);
        CHECK(format.sample_rate == 44'100);
        CHECK(format.channels.size() == 2);
        CHECK(format.channels[0] == AudioChannel::FrontLeft);
        CHECK(format.channels[1] == AudioChannel::FrontRight);
        CHECK(result.item->payload.buffer->bytes_per_sample() == 4);
        CHECK(result.item->payload.buffer->plane_count() == 1);
        CHECK(result.item->payload.duration.nanoseconds > 0);

        const auto plane = result.item->payload.buffer->plane(0);
        CHECK(plane.data != nullptr);
        CHECK(plane.size_bytes >=
            static_cast<std::size_t>(
                result.item->payload.buffer->sample_frame_count()) * 8U);

        saw_output = true;
        total_sample_frames += result.item->payload.buffer->sample_frame_count();
        total_duration_ns += result.item->payload.duration.nanoseconds;
    }

    CHECK(saw_output);
    CHECK(total_sample_frames == 441);
    CHECK(total_duration_ns >= 9'999'990);
    CHECK(total_duration_ns <= 10'000'010);
    CHECK(processor.state() == AudioProcessWorkerState::Drained);
    CHECK(processor.last_status() == AudioProcessStatus::Drained);

    processor.stop();
    decoder.stop();
    media.stop_decode_generation();

    // A new epoch reuses the worker but resets libswresample state, so delayed
    // samples from the previous generation cannot leak across seek.
    CHECK(media.seek(MediaTime{}));
    CHECK(media.start_decode_generation({false, true}));
    auto decoded_again = decoder.start(PlaybackEpoch{32});
    auto processed_again = processor.start(decoded_again);
    auto first = processed_again->pop();
    CHECK(first.has_item());
    CHECK(first.item->epoch.value() == 32);
    CHECK(first.item->payload.buffer != nullptr);
    CHECK(first.item->payload.buffer->format().sample_rate == 44'100);

    processor.stop();
    decoder.stop();
    media.stop_decode_generation();

    // Closing a bounded queue preserves already-buffered frames. Drain those
    // frames and verify every survivor belongs to the new epoch before Closed.
    for (;;) {
        auto remaining = processed_again->pop();
        if (remaining.status == QueueReadStatus::Closed) {
            break;
        }
        CHECK(remaining.has_item());
        CHECK(remaining.item->epoch.value() == 32);
        CHECK(remaining.item->timestamp == remaining.item->payload.timestamp);
        CHECK(remaining.item->payload.buffer != nullptr);
    }

    return 0;
}
