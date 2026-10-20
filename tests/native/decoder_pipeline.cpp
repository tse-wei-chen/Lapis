#include "core/decode_pump.hpp"
#include "core/decode_source.hpp"
#include <bdmv/player_c_api.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
struct SlowSource final : bdmv::core::IMediaDecodeSource {
    bool has_video() const noexcept override { return true; }
    bool has_audio() const noexcept override { return false; }
    bdmv::core::DecodeReadStatus next(bdmv::core::DecodedBuffer& frame, std::string&) override {
        std::this_thread::sleep_for(20ms);
        frame.pts_ns = cursor_++ * 1'000'000LL;
        frame.width = frame.height = 1;
        frame.bytes.assign(4, 1);
        return bdmv::core::DecodeReadStatus::Frame;
    }
    bool seek(std::int64_t ns, std::string&) override { cursor_ = ns / 1'000'000; return true; }
    std::int64_t cursor_{0};
};
}
int main() {
    // Unit exercise: race seek with an inflight decode; worker must discard
    // every frame of the prior generation, regardless of decoder lock order.
    {
        bdmv::core::DecodePump pump(std::make_unique<SlowSource>(), 9, 1, 2);
        pump.set_playing(true);
        std::this_thread::sleep_for(5ms);
        std::string error;
        assert(pump.seek(1'000'000'000LL, 2, error));
        bdmv::core::DecodedBuffer frame;
        bool got = false;
        for (int i = 0; i < 1000; ++i) {
            if (pump.try_read(frame) == bdmv::core::DecodePump::ReadStatus::Frame) {
                got = true; break;
            }
            std::this_thread::sleep_for(1ms);
        }
        assert(got && frame.epoch == 2 && frame.pts_ns >= 1'000'000'000LL);
        pump.set_playing(false);
    }

    bdmv_engine* engine = nullptr;
    const bdmv_engine_config config{sizeof(config), 0, BDMV_VIDEO_AUTO, BDMV_AUDIO_AUTO};
    assert(bdmv_engine_create(&config, &engine) == BDMV_OK);
    bdmv_session* session = nullptr;
    assert(bdmv_session_create(engine, &session) == BDMV_OK);
    assert(bdmv_session_open_uri(session, "test://frames") == BDMV_OK);
    bdmv_decoded_frame_info info{sizeof(info)};
    assert(bdmv_session_read_frame(session, &info, nullptr, 0) == BDMV_AGAIN);
    assert(bdmv_session_play(session) == BDMV_OK);
    bool ready = false;
    for (int i = 0; i < 1000; ++i) {
        auto rc = bdmv_session_read_frame(session, &info, nullptr, 0);
        if (rc == BDMV_BUFFER_TOO_SMALL) { ready = true; break; }
        assert(rc == BDMV_AGAIN);
        std::this_thread::sleep_for(1ms);
    }
    assert(ready);
    assert(info.required_bytes == 16 && info.kind == BDMV_DECODED_RGBA8);
    const auto before_epoch = info.epoch;
    std::vector<std::uint8_t> tiny(2);
    assert(bdmv_session_read_frame(session, &info, tiny.data(), tiny.size()) == BDMV_BUFFER_TOO_SMALL);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(info.required_bytes));
    assert(bdmv_session_read_frame(session, &info, buffer.data(), buffer.size()) == BDMV_OK);
    assert(buffer.front() == 0);
    assert(bdmv_session_seek(session, 120'000'000) == BDMV_OK);
    bool found = false;
    for (int i = 0; i < 1000; ++i) {
        auto rc = bdmv_session_read_frame(session, &info, nullptr, 0);
        if (rc == BDMV_BUFFER_TOO_SMALL) { found = true; break; }
        assert(rc == BDMV_AGAIN);
        std::this_thread::sleep_for(1ms);
    }
    assert(found && info.pts_ns >= 120'000'000 && info.epoch == before_epoch + 1);
    int decoded = 0;
    for (int i = 0; i < 2000; ++i) {
        auto rc = bdmv_session_read_frame(session, &info, nullptr, 0);
        if (rc == BDMV_END_OF_STREAM) break;
        if (rc == BDMV_AGAIN) { std::this_thread::sleep_for(1ms); continue; }
        assert(rc == BDMV_BUFFER_TOO_SMALL);
        assert(info.epoch == before_epoch + 1 && info.pts_ns >= 120'000'000);
        buffer.resize(static_cast<std::size_t>(info.required_bytes));
        assert(bdmv_session_read_frame(session, &info, buffer.data(), buffer.size()) == BDMV_OK);
        ++decoded;
    }
    assert(decoded == 10); // remaining 5 RGBA + 5 PCM blocks
    bdmv_session_snapshot snapshot{sizeof(snapshot)};
    assert(bdmv_session_get_snapshot(session, &snapshot) == BDMV_OK);
    assert(snapshot.transport == BDMV_ENDED && snapshot.epoch == before_epoch + 1);
    assert(bdmv_session_seek(session, 0) == BDMV_OK);
    assert(bdmv_session_play(session) == BDMV_OK); // ended -> paused after seek
    assert(bdmv_session_close(session) == BDMV_OK);
    assert(bdmv_session_read_frame(session, &info, buffer.data(), buffer.size()) == BDMV_INVALID_STATE);
    bdmv_session_destroy(session);
    bdmv_engine_destroy(engine);
    std::cout << "M1c decoder worker, C ABI frame exchange and seek fence passed\n";
}
