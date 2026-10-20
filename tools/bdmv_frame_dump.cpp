// Offline CPU diagnostic. NOT a video presenter or audio device output.
// Usage: bdmv_frame_dump <file-path-or-test-uri> <output-prefix>
// Emits the first decoded video frame as PPM, all returned PCM as S16LE raw.
#include <bdmv/player_c_api.h>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: bdmv_frame_dump <media_path|test://frames> <output_prefix>\n";
        return 2;
    }
    const bdmv_engine_config config{sizeof(config), 0, BDMV_VIDEO_SOFTWARE, BDMV_AUDIO_NULL};
    bdmv_engine* engine = nullptr;
    bdmv_session* session = nullptr;
    if (bdmv_engine_create(&config, &engine) != BDMV_OK ||
        bdmv_session_create(engine, &session) != BDMV_OK) return 3;
    auto cleanup = [&] {
        bdmv_session_destroy(session);
        bdmv_engine_destroy(engine);
    };
    const auto opened = bdmv_session_open_uri(session, argv[1]);
    if (opened != BDMV_OK) { std::cerr << "cannot open: " << opened << '\n'; cleanup(); return 4; }
    if (bdmv_session_play(session) != BDMV_OK) { cleanup(); return 5; }
    const std::string prefix = argv[2];
    std::ofstream pcm(prefix + ".s16le", std::ios::binary);
    if (!pcm) { cleanup(); return 6; }
    std::uint32_t videos = 0, audios = 0;
    bdmv_decoded_frame_info info{sizeof(info)};
    std::vector<std::uint8_t> data;
    bool eof = false;
    for (int idle = 0; idle < 5000;) {
        const auto status = bdmv_session_read_frame(session, &info, nullptr, 0);
        if (status == BDMV_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ++idle;
            continue;
        }
        if (status == BDMV_END_OF_STREAM) { eof = true; break; }
        if (status != BDMV_BUFFER_TOO_SMALL || info.required_bytes > 128ULL*1024*1024) {
            std::cerr << "decode error: " << status << '\n'; cleanup(); return 7;
        }
        data.resize(static_cast<std::size_t>(info.required_bytes));
        if (bdmv_session_read_frame(session, &info, data.data(), data.size()) != BDMV_OK) {
            cleanup(); return 8;
        }
        idle = 0;
        if (info.kind == BDMV_DECODED_RGBA8) {
            if (videos++ == 0) {
                if (info.width == 0 || info.height == 0 ||
                    info.required_bytes != static_cast<std::uint64_t>(info.width)*info.height*4) {
                    cleanup(); return 9;
                }
                std::ofstream image(prefix + ".ppm", std::ios::binary);
                image << "P6\n" << info.width << ' ' << info.height << "\n255\n";
                for (std::size_t i = 0; i < data.size(); i += 4)
                    image.write(reinterpret_cast<const char*>(data.data() + i), 3);
                if (!image) { cleanup(); return 10; }
            }
        } else if (info.kind == BDMV_DECODED_PCM_S16_STEREO) {
            pcm.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
            ++audios;
        }
    }
    pcm.close();
    bdmv_session_close(session);
    cleanup();
    std::cout << "video_frames=" << videos << " audio_blocks=" << audios
              << " end_of_stream=" << (eof ? "yes" : "no") << '\n';
    return eof && videos && audios && pcm ? 0 : 11;
}
