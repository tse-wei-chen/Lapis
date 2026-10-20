#include "core/decode_source.hpp"
#include "core/epoch_queue.hpp"
#include "core/media_time.hpp"
#include "core/platform_interfaces.hpp"
#include "core/playback_clock.hpp"
#include "core/presentation_timing.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>

static void require(bool pass, const char* desc) {
    if (!pass) throw std::runtime_error(desc);
}

int main() {
    using namespace bdmv::core;
    require(timebase_to_ns(90'000, 1, 90'000).value() == 1'000'000'000,
            "90 kHz conversion");
    require(!timebase_to_ns(1, 0, 90'000).has_value(), "zero numerator rejected");
    require(!rgba_buffer_size(0, 128).has_value(), "zero image size rejected");
    require(rgba_buffer_size(64, 32).value() == 8192, "RGBA allocation size");

    EpochQueue<int> queue(2);
    queue.reset(7, 1);
    require(queue.try_push({7, 1, 0, 10}), "push first epoch");
    queue.reset(7, 2);
    require(!queue.try_pop(), "old seek frames must disappear");
    require(!queue.try_push({7, 1, 0, 20}), "old epoch rejected");
    require(queue.try_push({7, 2, 20, 30}), "current epoch accepted");
    require(queue.try_pop()->value == 30, "current frame consumed");

    PlaybackClock clock;
    clock.reanchor(1'000'000'000, 50);
    clock.play(50);
    require(clock.position(1050) == 1'000'001'000, "play clock advances");
    clock.pause(1050);
    require(clock.position(4050) == 1'000'001'000, "pause clock stable");
    clock.seek(0, 4050);
    require(clock.position(5050) == 0, "paused seek remains stable");

    require(VideoTiming::decide(90'000'000, 0).action == VideoDisposition::Wait,
            "early frame waits");
    require(VideoTiming::decide(0, 100'000'000).action == VideoDisposition::Drop,
            "late frame drops");
    require(estimated_audio_playhead_ns(1'000'000'000, 192'000, 48000, 2, 2) == 0,
            "one second queued PCM");

    std::cout << "portable core contract smoke: OK\n";
}
