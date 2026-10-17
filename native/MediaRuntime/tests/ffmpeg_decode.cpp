#include "core/graph_plan.hpp"
#include "session/playback_media_session.hpp"

#include <cassert>

int main() {
    using namespace lapis::media;

    PlaybackMediaSession session;
    const MediaLocator locator{MediaLocatorKind::File, LAPIS_TEST_MEDIA_PATH};
    const auto open_result = session.open(locator);

    assert(open_result.ok());
    assert(open_result.has_video);
    assert(open_result.video_width == 2);
    assert(open_result.video_height == 2);

    const auto* plan = session.graph_plan();
    assert(plan != nullptr);
    assert(plan->nodes().size() == 3);
    assert(plan->edges().size() == 2);
    assert(plan->nodes()[0].kind == GraphNodeKind::Source);
    assert(plan->nodes()[1].kind == GraphNodeKind::Demuxer);
    assert(plan->nodes()[2].kind == GraphNodeKind::VideoDecoder);

    const auto frame = session.decode_next_video_frame();
    assert(frame.has_value());
    assert(frame->surface != nullptr);
    assert(frame->surface->kind() == VideoSurfaceKind::SystemMemory);
    assert(frame->surface->width() == 2);
    assert(frame->surface->height() == 2);

    return 0;
}
