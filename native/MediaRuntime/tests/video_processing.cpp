#include "execution/video_decode_worker.hpp"
#include "execution/video_process_worker.hpp"
#include "process/ffmpeg/ffmpeg_video_frame_converter.hpp"
#include "session/playback_media_session.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main() {
    using namespace lapis::media;
    using namespace std::chrono_literals;

    PlaybackMediaSession media;
    const MediaLocator locator{MediaLocatorKind::File, LAPIS_TEST_MEDIA_PATH};
    CHECK(media.open(locator).ok());

    VideoDecodeWorker decoder(media, 1);
    VideoProcessWorker processor(
        std::make_unique<ffmpeg::FfmpegVideoFrameConverter>(),
        1);

    auto decoded = decoder.start(PlaybackEpoch{42});
    auto processed = processor.start(decoded);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    QueueReadResult<TimedVideoFrame> result;
    do {
        result = processed->try_pop();
        if (result.status == QueueReadStatus::Empty) {
            std::this_thread::sleep_for(1ms);
        }
    } while (result.status == QueueReadStatus::Empty &&
             std::chrono::steady_clock::now() < deadline);

    CHECK(result.has_item());
    CHECK(result.item->epoch.value() == 42);
    CHECK(result.item->payload.surface != nullptr);
    CHECK(result.item->payload.surface->kind() == VideoSurfaceKind::SystemMemory);
    CHECK(result.item->payload.surface->pixel_format() == VideoPixelFormat::Bgra8);
    CHECK(result.item->payload.surface->width() == 2);
    CHECK(result.item->payload.surface->height() == 2);
    CHECK(result.item->timestamp == result.item->payload.timestamp);

    const auto* surface = dynamic_cast<const ISystemMemoryVideoSurface*>(
        result.item->payload.surface.get());
    CHECK(surface != nullptr);
    CHECK(surface->plane_count() == 1);
    const auto plane = surface->plane(0);
    CHECK(plane.data != nullptr);
    CHECK(plane.stride >= 8);
    CHECK((plane.stride % 4) == 0);

    processor.stop();
    decoder.stop();
    CHECK(processor.last_status() == VideoProcessStatus::Processed);
    CHECK(processor.state() == VideoProcessWorkerState::Drained ||
          processor.state() == VideoProcessWorkerState::Stopped);

    return 0;
}
