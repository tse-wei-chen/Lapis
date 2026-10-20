#include "core/presentation_timing.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <limits>
#include <iostream>
using namespace bdmv::core;
int main() {
    assert(VideoTiming::decide(100'000'000, 90'000'000).action == VideoDisposition::Wait);
    assert(VideoTiming::decide(100'000'000, 90'000'000).wait_ns == 10'000'000);
    assert(VideoTiming::decide(100'000'000, 100'000'000).action == VideoDisposition::Present);
    assert(VideoTiming::decide(100'000'000, 179'000'000).action == VideoDisposition::Present);
    assert(VideoTiming::decide(100'000'000, 181'000'000).action == VideoDisposition::Drop);
    assert(VideoTiming::decide(-1, 180'000'000).action == VideoDisposition::Present);
    assert(VideoTiming::decide(INT64_MAX, 0).wait_ns == 1'000'000'000);
    assert(VideoTiming::decide(0, INT64_MAX).action == VideoDisposition::Drop);
    assert(estimated_audio_playhead_ns(1'000'000'000, 48'000 * 2 * 2, 48'000, 2, 2) == 0);
    assert(estimated_audio_playhead_ns(1'000'000'000, 480 * 4, 48'000, 2, 2) == 990'000'000);
    assert(estimated_audio_playhead_ns(1'000'000'000, 0, 48'000, 2, 2) == 1'000'000'000);
    assert(estimated_audio_playhead_ns(0, 4000, 0, 2, 2) == -1);
    std::cout << "Video scheduling and queued-audio position estimates passed\n";
}
