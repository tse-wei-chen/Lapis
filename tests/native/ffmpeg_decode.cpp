#include "adapters/ffmpeg/ffmpeg_decoder.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::string error;
    auto decoder = bdmv::adapters::FfmpegDecoder::open_file(argv[1], error);
    if (!decoder) { std::cerr << error << '\n'; return 3; }
    if (!decoder->has_video() || !decoder->has_audio()) return 4;
    auto inspect = [&]() -> int {
        int video = 0, audio = 0;
        std::uint64_t video_bytes = 0, audio_bytes = 0;
        for (int i = 0; i < 10000; ++i) {
            bdmv::core::DecodedBuffer frame;
            auto state = decoder->next(frame, error);
            if (state == bdmv::core::DecodeReadStatus::EndOfStream) {
                if (video < 2 || audio < 1) return 5;
                std::cout << "video=" << video << " rgba_bytes=" << video_bytes
                          << " audio=" << audio << " pcm_bytes=" << audio_bytes << '\n';
                return 0;
            }
            if (state == bdmv::core::DecodeReadStatus::Error) {
                std::cerr << error << '\n'; return 6;
            }
            if (frame.kind == bdmv::core::DecodedKind::VideoRgba8) {
                if (frame.bytes.size() != static_cast<std::size_t>(frame.width)*frame.height*4)
                    return 7;
                video_bytes += frame.bytes.size(); ++video;
            } else {
                if (frame.channels != 2 || frame.sample_rate != 48000 || frame.bytes.size()%4)
                    return 8;
                audio_bytes += frame.bytes.size(); ++audio;
            }
        }
        return 9;
    };
    if (int rc = inspect()) return rc;
    if (!decoder->seek(0, error)) { std::cerr << error << '\n'; return 10; }
    return inspect();
}
