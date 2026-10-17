#include "core/graph_plan.hpp"
#include "session/playback_media_session.hpp"

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

    PlaybackMediaSession session;
    const MediaLocator locator{MediaLocatorKind::File, LAPIS_TEST_MEDIA_PATH};
    const auto open_result = session.open(locator);

    CHECK(open_result.ok());
    CHECK(open_result.has_video);
    CHECK(open_result.video_width == 2);
    CHECK(open_result.video_height == 2);

    const auto* plan = session.graph_plan();
    CHECK(plan != nullptr);
    CHECK(plan->nodes().size() == 3);
    CHECK(plan->edges().size() == 2);
    CHECK(plan->nodes()[0].kind == GraphNodeKind::Source);
    CHECK(plan->nodes()[1].kind == GraphNodeKind::Demuxer);
    CHECK(plan->nodes()[2].kind == GraphNodeKind::VideoDecoder);

    // Media open prepares decoders only. Routing starts explicitly when a
    // decode consumer owns the generation.
    CHECK(!session.decode_next_video_frame().has_value());
    CHECK(session.start_decode_generation(MediaDecodeSelection{true, false}));

    const auto frame = session.decode_next_video_frame();
    CHECK(frame.has_value());
    CHECK(frame->surface != nullptr);
    CHECK(frame->surface->kind() == VideoSurfaceKind::SystemMemory);
    CHECK(frame->surface->width() == 2);
    CHECK(frame->surface->height() == 2);

    session.stop_decode_generation();
    return 0;
}
