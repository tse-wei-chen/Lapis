#include "demux/ffmpeg/ffmpeg_packet_router.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/error.h>
}

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "CHECK failed: " #condition << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace {

struct PacketEvent final {
    int stream_index{};
    std::uint8_t value{};
};

class FakePacketSource final : public lapis::media::ffmpeg::IFfmpegPacketSource {
public:
    explicit FakePacketSource(std::vector<PacketEvent> events)
        : events_(std::move(events)) {}

    int read_packet(AVPacket& packet) override {
        if (cursor_ >= events_.size()) {
            return AVERROR_EOF;
        }

        av_packet_unref(&packet);
        if (av_new_packet(&packet, 1) < 0) {
            return AVERROR(ENOMEM);
        }

        const auto event = events_[cursor_++];
        packet.stream_index = event.stream_index;
        packet.data[0] = event.value;
        return 0;
    }

private:
    std::vector<PacketEvent> events_;
    std::size_t cursor_{};
};

bool consume_stream(
    lapis::media::ffmpeg::FfmpegPacketRouter& router,
    int stream_index,
    std::array<std::uint8_t, 3>& values) {
    using namespace lapis::media::ffmpeg;

    for (auto& value : values) {
        auto result = router.read(stream_index);
        if (!result.has_packet()) {
            return false;
        }
        if (result.packet->stream_index() != stream_index ||
            result.packet->get().size != 1) {
            return false;
        }
        value = result.packet->get().data[0];
    }
    return true;
}

} // namespace

int main() {
    using namespace lapis::media::ffmpeg;
    using namespace std::chrono_literals;

    FakePacketSource source({
        {1, 10}, {2, 20}, {1, 11}, {2, 21}, {1, 12}, {2, 22}
    });
    FfmpegPacketRouter router(source, {1, 2}, 1);
    router.start();

    CHECK(router.read(99).status == FfmpegPacketReadStatus::UnknownStream);

    std::array<std::uint8_t, 3> video{};
    std::array<std::uint8_t, 3> audio{};
    auto video_reader = std::async(std::launch::async, [&] {
        return consume_stream(router, 1, video);
    });
    auto audio_reader = std::async(std::launch::async, [&] {
        return consume_stream(router, 2, audio);
    });

    CHECK(video_reader.wait_for(2s) == std::future_status::ready);
    CHECK(audio_reader.wait_for(2s) == std::future_status::ready);
    CHECK(video_reader.get());
    CHECK(audio_reader.get());
    CHECK((video == std::array<std::uint8_t, 3>{10, 11, 12}));
    CHECK((audio == std::array<std::uint8_t, 3>{20, 21, 22}));

    const auto drain_deadline = std::chrono::steady_clock::now() + 2s;
    while (router.state() == FfmpegPacketRouterState::Running &&
           std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(router.state() == FfmpegPacketRouterState::Drained);
    CHECK(router.read(1).status == FfmpegPacketReadStatus::Drained);
    CHECK(router.read(2).status == FfmpegPacketReadStatus::Drained);
    router.stop();

    // Some MP4 muxers place about a second of video packets ahead of
    // their corresponding audio packets. The default queues must let the demux
    // pump reach audio even while video is temporarily backpressured.
    std::vector<PacketEvent> skewed_events;
    for (std::uint8_t value = 0; value < 64; ++value) {
        skewed_events.push_back({1, value});
    }
    skewed_events.push_back({2, 99});
    FakePacketSource skewed_source(std::move(skewed_events));
    FfmpegPacketRouter skewed_router(skewed_source, {1, 2});
    skewed_router.start();

    auto skewed_audio_reader = std::async(std::launch::async, [&] {
        return skewed_router.read(2);
    });
    const bool audio_ready =
        skewed_audio_reader.wait_for(2s) == std::future_status::ready;
    if (!audio_ready) {
        skewed_router.stop();
    }
    CHECK(audio_ready);
    const auto skewed_audio = skewed_audio_reader.get();
    CHECK(skewed_audio.has_packet());
    CHECK(skewed_audio.packet->get().data[0] == 99);
    for (std::uint8_t expected = 0; expected < 64; ++expected) {
        const auto packet = skewed_router.read(1);
        CHECK(packet.has_packet());
        CHECK(packet.packet->get().data[0] == expected);
    }
    skewed_router.stop();

    // Stop must release a demux pump blocked by bounded backpressure.
    std::vector<PacketEvent> blocked_events;
    for (std::uint8_t value = 0; value < 16; ++value) {
        blocked_events.push_back({7, value});
    }
    FakePacketSource blocked_source(std::move(blocked_events));
    FfmpegPacketRouter blocked_router(blocked_source, {7}, 1);
    blocked_router.start();
    std::this_thread::sleep_for(20ms);

    const auto stop_started = std::chrono::steady_clock::now();
    blocked_router.stop();
    CHECK(std::chrono::steady_clock::now() - stop_started < 1s);
    CHECK(blocked_router.state() == FfmpegPacketRouterState::Stopped);

    return 0;
}
