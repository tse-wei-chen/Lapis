#include <bdmv/player_c_api.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    bdmv_abi_version version{sizeof(bdmv_abi_version), 0, 0, 0};
    assert(bdmv_get_abi_version(&version) == BDMV_OK);
    assert(version.major == BDMV_ABI_MAJOR);

    bdmv_engine* engine = nullptr;
    const bdmv_engine_config config{sizeof(bdmv_engine_config), 0, BDMV_VIDEO_AUTO, BDMV_AUDIO_AUTO};
    assert(bdmv_engine_create(&config, &engine) == BDMV_OK && engine);
    bdmv_engine_capabilities caps{sizeof(bdmv_engine_capabilities), 0, 0, 0, 0, 0, 0};
    assert(bdmv_engine_query_capabilities(engine, &caps) == BDMV_OK);
    assert(caps.file_source == 0 && caps.bluray_source == 0);

    bdmv_session* session = nullptr;
    assert(bdmv_session_create(engine, &session) == BDMV_OK && session);
    assert(bdmv_session_play(session) == BDMV_INVALID_STATE);
    assert(bdmv_session_open_uri(session, "file:///movie.mkv") == BDMV_UNSUPPORTED);
    const auto open_status = bdmv_session_open_uri(session, "test://synthetic");
    if (caps.test_source) {
        assert(open_status == BDMV_OK);
    } else {
        assert(open_status == BDMV_UNSUPPORTED);
        bdmv_session_destroy(session);
        bdmv_engine_destroy(engine);
        std::cout << "native ABI smoke passed (without diagnostic source)\n";
        return 0;
    }
    bdmv_session_snapshot before{sizeof(bdmv_session_snapshot), 0, 0, 0, 0, 0, 0, 0, 0};
    assert(bdmv_session_get_snapshot(session, &before) == BDMV_OK);
    assert(before.transport == BDMV_READY && before.epoch == 1 && before.session_id != 0);
    assert(bdmv_session_play(session) == BDMV_OK);

    // Concurrent control calls on one session are serialized at the native boundary.
    constexpr int workers = 4, seeks_per_worker = 100;
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (int t = 0; t < workers; ++t) {
        threads.emplace_back([session, t]() {
            for (int i = 0; i < seeks_per_worker; ++i) {
                assert(bdmv_session_seek(session,
                       static_cast<std::int64_t>(t * seeks_per_worker + i) * 1000000000LL) == BDMV_OK);
            }
        });
    }
    for (auto& t : threads) t.join();

    bdmv_session_snapshot after{sizeof(bdmv_session_snapshot), 0, 0, 0, 0, 0, 0, 0, 0};
    assert(bdmv_session_get_snapshot(session, &after) == BDMV_OK);
    assert(after.epoch == before.epoch + workers * seeks_per_worker);
    assert(after.transport == BDMV_PLAYING);
    assert(bdmv_session_pause(session) == BDMV_OK);
    assert(bdmv_session_close(session) == BDMV_OK);
    assert(bdmv_session_get_snapshot(session, &after) == BDMV_OK);
    assert(after.epoch == before.epoch + workers * seeks_per_worker + 1);
    assert(after.transport == BDMV_CLOSED);

    bdmv_session_destroy(session);
    bdmv_engine_destroy(engine);
    std::cout << "native ABI and epoch smoke passed\n";
}
