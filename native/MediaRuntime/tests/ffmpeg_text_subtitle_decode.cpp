#include "session/playback_media_session.hpp"

#include <cstdint>
#include <iostream>
#include <string>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed: " #condition << " line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

int main() {
    using namespace lapis::media;
    PlaybackMediaSession session;
    CHECK(session.open(MediaLocator{
        MediaLocatorKind::File, LAPIS_TEST_SUBTITLE_MEDIA_PATH
    }).ok());

    const auto found = session.subtitle_streams();
    CHECK(found.size() == 1);
    CHECK(found[0].stream_index == 1); // video stream is index 0
    CHECK(found[0].kind == SubtitleTrackKind::Text);
    CHECK(found[0].codec_name == "subrip");
    CHECK(found[0].language == "eng");

    // Merely discovering a subtitle must not add an unconsumed packet
    // queue to normal video playback. Opt in explicitly.
    CHECK(!session.decode_next_text_subtitle_cue().has_value());
    CHECK(!session.start_decode_generation(MediaDecodeSelection{false, false}));
    CHECK(!session.start_decode_generation(MediaDecodeSelection{false, false, 0}));
    CHECK(!session.start_decode_generation(MediaDecodeSelection{false, false, 500}));

    CHECK(session.start_decode_generation(MediaDecodeSelection{false, false, 1}));
    const auto first = session.decode_next_text_subtitle_cue();
    CHECK(first.has_value());
    CHECK(first->valid());
    CHECK(first->text == "Hello embedded text");
    CHECK(first->start.nanoseconds == 1'000'000'000);
    CHECK(first->end.nanoseconds == 3'000'000'000);

    const auto second = session.decode_next_text_subtitle_cue();
    CHECK(second.has_value());
    CHECK(second->text == "Second cue");
    CHECK(second->start.nanoseconds == 3'000'000'000);
    CHECK(second->end.nanoseconds == 5'000'000'000);
    CHECK(!session.decode_next_text_subtitle_cue().has_value());
    session.stop_decode_generation();
    CHECK(!session.decode_next_text_subtitle_cue().has_value());

    // Seek retires the old router, resets decoder state and starts a new
    // subtitle generation. An old subtitle MUST NOT leak into this read.
    CHECK(session.seek(MediaTime{0}));
    CHECK(session.start_decode_generation(MediaDecodeSelection{false, false, 1}));
    const auto after_seek = session.decode_next_text_subtitle_cue();
    CHECK(after_seek.has_value());
    CHECK(after_seek->text == "Hello embedded text");
    CHECK(after_seek->start.nanoseconds == first->start.nanoseconds);
    session.stop_decode_generation();

    // Both video and selected text subtitle can share the packet router.
    CHECK(session.seek(MediaTime{0}));
    CHECK(session.start_decode_generation(MediaDecodeSelection{true, false, 1}));
    CHECK(session.decode_next_video_frame().has_value());
    const auto alongside_video = session.decode_next_text_subtitle_cue();
    CHECK(alongside_video.has_value());
    CHECK(alongside_video->text == "Hello embedded text");
    session.stop_decode_generation();
    std::cout << "Real embedded Matroska SubRip decode + seek fence passed\n";
    return 0;
}
