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

    // The custom FFmpeg AVIO source must remain readable after a nonzero
    // seek. This catches a second demux flush accidentally discarding the
    // seeker's queued state on small files.
    PlaybackMediaSession seekable;
    const MediaLocator long_clip{
        MediaLocatorKind::File, LAPIS_TEST_NONZERO_SEEK_MEDIA_PATH
    };
    CHECK(seekable.open(long_clip).ok());
    CHECK(seekable.start_decode_generation(MediaDecodeSelection{true, false}));
    CHECK(seekable.decode_next_video_frame().has_value());
    CHECK(seekable.seek(MediaTime{250'000'000}));
    CHECK(seekable.start_decode_generation(MediaDecodeSelection{true, false}));
    const auto after_forward_seek = seekable.decode_next_video_frame();
    CHECK(after_forward_seek.has_value());
    CHECK(after_forward_seek->timestamp.nanoseconds >= 0);
    CHECK(seekable.seek(MediaTime{}));
    CHECK(seekable.start_decode_generation(MediaDecodeSelection{true, false}));
    const auto after_backward_seek = seekable.decode_next_video_frame();
    CHECK(after_backward_seek.has_value());
    CHECK(after_backward_seek->timestamp.nanoseconds >= 0);
    seekable.stop_decode_generation();

    // Regression for Y4M: the stream may be entirely prefetched by custom
    // AVIO. Seeking must produce real decoded frames rather than an immediate
    // false Ended state after av_seek_frame() reports success.
    PlaybackMediaSession y4m;
    const MediaLocator raw_clip{
        MediaLocatorKind::File, LAPIS_TEST_Y4M_SEEK_MEDIA_PATH
    };
    CHECK(y4m.open(raw_clip).ok());
    CHECK(y4m.start_decode_generation(MediaDecodeSelection{true, false}));
    CHECK(y4m.decode_next_video_frame().has_value());
    CHECK(y4m.seek(MediaTime{250'000'000}));
    CHECK(y4m.start_decode_generation(MediaDecodeSelection{true, false}));
    const auto y4m_forward = y4m.decode_next_video_frame();
    CHECK(y4m_forward.has_value());
    CHECK(y4m_forward->timestamp.nanoseconds >= 0);
    CHECK(y4m.seek(MediaTime{}));
    CHECK(y4m.start_decode_generation(MediaDecodeSelection{true, false}));
    const auto y4m_back = y4m.decode_next_video_frame();
    CHECK(y4m_back.has_value());
    CHECK(y4m_back->timestamp.nanoseconds >= 0);
    // Failed seeks must not silently publish Ended or poison a later seek.
    CHECK(!y4m.seek(MediaTime{100'000'000'000}));
    CHECK(y4m.seek(MediaTime{}));
    CHECK(y4m.start_decode_generation(MediaDecodeSelection{true, false}));
    CHECK(y4m.decode_next_video_frame().has_value());
    y4m.stop_decode_generation();
    return 0;
}
