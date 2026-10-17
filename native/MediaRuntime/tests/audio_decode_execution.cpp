#include "core/graph_plan.hpp"
#include "execution/audio_decode_worker.hpp"
#include "session/playback_media_session.hpp"

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

    PlaybackMediaSession media;
    const MediaLocator locator{MediaLocatorKind::File, LAPIS_TEST_AUDIO_PATH};
    const auto opened = media.open(locator);
    CHECK(opened.ok());
    CHECK(!opened.has_video);
    CHECK(opened.has_audio);
    CHECK(opened.audio_sample_rate == 48'000);
    CHECK(opened.audio_channel_count == 2);
    CHECK(!media.decode_next_audio_frame().has_value());
    CHECK(!media.start_decode_generation(MediaDecodeSelection{true, false}));

    const auto* graph = media.graph_plan();
    CHECK(graph != nullptr);
    bool found_audio_decoder{};
    for (const auto& node : graph->nodes()) {
        if (node.kind == GraphNodeKind::AudioDecoder) {
            found_audio_decoder = true;
        }
    }
    CHECK(found_audio_decoder);

    CHECK(media.start_decode_generation(MediaDecodeSelection{false, true}));
    CHECK(!media.start_decode_generation(MediaDecodeSelection{false, true}));

    AudioDecodeWorker worker(media, 1);
    auto stream = worker.start(PlaybackEpoch{11});
    CHECK(stream->epoch().value() == 11);
    CHECK(stream->capacity() == 1);

    std::uint64_t total_frames{};
    for (;;) {
        auto result = stream->pop();
        if (result.status == QueueReadStatus::Closed) {
            break;
        }
        CHECK(result.has_item());
        CHECK(result.item->epoch.value() == 11);
        CHECK(result.item->timestamp == result.item->payload.timestamp);
        CHECK(result.item->payload.buffer != nullptr);
        total_frames += result.item->payload.buffer->sample_frame_count();
    }
    CHECK(total_frames == 480);
    CHECK(worker.state() == AudioDecodeWorkerState::Drained);
    worker.stop();
    media.stop_decode_generation();

    CHECK(media.seek(MediaTime{}));
    CHECK(media.start_decode_generation(MediaDecodeSelection{false, true}));
    auto next = worker.start(PlaybackEpoch{12});
    auto first = next->pop();
    CHECK(first.has_item());
    CHECK(first.item->epoch.value() == 12);
    CHECK(first.item->payload.buffer != nullptr);
    worker.stop();
    media.stop_decode_generation();
    CHECK(next->pop().status == QueueReadStatus::Closed);

    return 0;
}
