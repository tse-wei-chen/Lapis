#include "core/decoded_media.hpp"
#include "core/epoch_queue.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

int main() {
    using namespace bdmv::core;
    assert(!rgba_buffer_size(0, 1080));
    assert(!rgba_buffer_size(16385, 1080));
    assert(rgba_buffer_size(1920, 1080) == 8'294'400);
    auto decoded = DecodedBuffer{.kind=DecodedKind::VideoRgba8};
    decoded.session_id = 7;
    decoded.epoch = 12;
    decoded.width = 2;
    decoded.height = 2;
    decoded.pts_ns = 42;
    decoded.bytes.resize(16);
    EpochQueue<DecodedBuffer> queue(2);
    queue.reset(7, 12);
    assert(matches_epoch(decoded, 7, 12));
    assert(!matches_epoch(decoded, 7, 11));
    assert(queue.try_push({7, 12, 42, decoded}));
    queue.reset(7, 13); // Concurrent seek/close transaction invalidates older frames.
    assert(!queue.try_pop());
    assert(!queue.try_push({7, 12, 42, decoded}));
    decoded.epoch = 13;
    assert(queue.try_push({7, 13, 42, decoded}));
    assert(queue.try_pop()->value.bytes.size() == 16);
    std::cout << "decoded media DTOs and epoch barriers passed\n";
}
