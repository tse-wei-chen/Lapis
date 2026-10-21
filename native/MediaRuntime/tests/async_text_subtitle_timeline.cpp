#include "session/async_text_subtitle_timeline.hpp"
#include "session/playback_media_session.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {
using namespace std::chrono_literals;
using lapis::media::AsyncTextSubtitleTimeline;
using lapis::media::MediaLocator;
using lapis::media::MediaLocatorKind;
using lapis::media::TextSubtitleTimelineState;

bool await_terminal(const AsyncTextSubtitleTimeline& task,
                    lapis::media::TextSubtitleTimelineSnapshot& snapshot) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        snapshot = task.snapshot();
        if (snapshot.state != TextSubtitleTimelineState::Loading) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
} // namespace

int main() {
    const MediaLocator source{MediaLocatorKind::File,
                              LAPIS_TEST_SUBTITLE_MEDIA_PATH};

    // Actual demux/decode runs on its own worker and independent file handle.
    AsyncTextSubtitleTimeline first(source, 1);
    lapis::media::TextSubtitleTimelineSnapshot result{};
    CHECK(await_terminal(first, result));
    CHECK(result.state == TextSubtitleTimelineState::Ready);
    CHECK(result.cues.size() == 2);
    CHECK(result.cues[0].start.nanoseconds == 1'000'000'000);
    CHECK(result.cues[0].end.nanoseconds == 3'000'000'000);
    CHECK(result.cues[0].text == "Hello embedded text");
    CHECK(result.cues[1].start.nanoseconds == 3'000'000'000);
    CHECK(result.cues[1].end.nanoseconds == 5'000'000'000);
    CHECK(result.cues[1].text == "Second cue");
    const auto page = first.read_page(1, 1);
    CHECK(page.state == TextSubtitleTimelineState::Ready);
    CHECK(page.total == 2 && page.cues.size() == 1);
    CHECK(page.cues[0].text == "Second cue");
    CHECK(first.read_page(2, 1).cues.empty());

    // Independent A/V playback decoder stays functional: this subtitle
    // consumer did not steal packets from the playback router.
    lapis::media::PlaybackMediaSession playback;
    CHECK(playback.open(source).ok());
    CHECK(playback.start_decode_generation(
        lapis::media::MediaDecodeSelection{true, false}));
    CHECK(playback.decode_next_video_frame().has_value());
    playback.stop_decode_generation();

    // Cancelling is a generation barrier, even if the short fixture finished
    // before cancellation. No completed cue can reappear afterward.
    first.cancel();
    CHECK(first.snapshot().state == TextSubtitleTimelineState::Cancelled);
    CHECK(first.snapshot().cues.empty());
    std::this_thread::sleep_for(20ms);
    CHECK(first.snapshot().state == TextSubtitleTimelineState::Cancelled);

    // A fresh generation reopens and reads its own media source and cannot
    // observe cues from the retired generation.
    AsyncTextSubtitleTimeline next(source, 1);
    CHECK(await_terminal(next, result));
    CHECK(result.state == TextSubtitleTimelineState::Ready);
    CHECK(result.cues.size() == 2);

    AsyncTextSubtitleTimeline invalid_stream(source, 900);
    CHECK(await_terminal(invalid_stream, result));
    CHECK(result.state == TextSubtitleTimelineState::Failed);
    CHECK(result.cues.empty());

    AsyncTextSubtitleTimeline invalid_path(
        MediaLocator{MediaLocatorKind::File,
                     "/not-a-real-lapis-subtitle-fixture-4242.mkv"}, 1);
    CHECK(await_terminal(invalid_path, result));
    CHECK(result.state == TextSubtitleTimelineState::Failed);
    CHECK(result.cues.empty());

    AsyncTextSubtitleTimeline forbidden_remote(
        MediaLocator{MediaLocatorKind::Uri, "https://example.org/clip.mkv"}, 1);
    CHECK(await_terminal(forbidden_remote, result));
    CHECK(result.state == TextSubtitleTimelineState::Failed);
    CHECK(result.cues.empty());

    CHECK(AsyncTextSubtitleTimeline::MaxCues == 4096);
    CHECK(AsyncTextSubtitleTimeline::MaxTextBytes == 4 * 1024 * 1024);
    std::cout << "async embedded subtitle decode, cancellation and A/V isolation passed\n";
    return 0;
}
