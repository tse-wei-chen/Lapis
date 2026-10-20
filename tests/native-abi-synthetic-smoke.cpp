#include <bdmv/player_c_api.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "check failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main() {
    bdmv_abi_version version{sizeof(bdmv_abi_version), 0, 0, 0};
    CHECK(bdmv_get_abi_version(&version) == BDMV_OK);
    CHECK(version.major == 1 && version.minor >= 1);
    bdmv_engine* engine = nullptr;
    bdmv_engine_config config{sizeof(bdmv_engine_config), 0, BDMV_VIDEO_AUTO, BDMV_AUDIO_AUTO};
    CHECK(bdmv_engine_create(&config, &engine) == BDMV_OK);
    CHECK(engine != nullptr);
    bdmv_session* session = nullptr;
    CHECK(bdmv_session_create(engine, &session) == BDMV_OK);
    CHECK(session != nullptr);
    CHECK(bdmv_session_open_uri(session, "test://frames") == BDMV_OK);
    CHECK(bdmv_session_play(session) == BDMV_OK);
    std::array<std::uint8_t, 8192> data{};
    unsigned frames = 0;
    std::uint64_t first_epoch = 0;
    for (unsigned attempt = 0; attempt < 4000 && frames < 16; ++attempt) {
        bdmv_decoded_frame_info info{};
        info.struct_size = sizeof(info);
        auto status = bdmv_session_read_frame(session, &info, data.data(), data.size());
        if (status == BDMV_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        CHECK(status == BDMV_OK);
        CHECK(info.session_id > 0 && info.required_bytes > 0);
        if (frames == 0) first_epoch = info.epoch;
        CHECK(info.epoch == first_epoch);
        CHECK(info.kind == BDMV_DECODED_RGBA8 || info.kind == BDMV_DECODED_PCM_S16_STEREO);
        ++frames;
    }
    CHECK(frames == 16);
    CHECK(bdmv_session_pause(session) == BDMV_OK);
    CHECK(bdmv_session_seek(session, 0) == BDMV_OK);
    bdmv_session_snapshot snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    CHECK(bdmv_session_get_snapshot(session, &snapshot) == BDMV_OK);
    CHECK(snapshot.epoch > first_epoch);
    CHECK(bdmv_session_close(session) == BDMV_OK);
    bdmv_session_destroy(session);
    bdmv_engine_destroy(engine);
    std::cout << "Native C ABI synthetic decode/epoch smoke: PASS (16 frames)\n";
}
