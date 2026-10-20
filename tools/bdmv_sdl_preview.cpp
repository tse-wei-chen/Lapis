// M1d developer preview only: a separate SDL2 runtime-loaded output adapter.
// Does NOT belong to MediaRuntime, and is NOT the final GPU/audio renderer.
// Loads the published SDL2 C ABI at runtime so a headless build does not
// require SDL2 developer headers. In production use the official SDL2 SDK.
#include <bdmv/player_c_api.h>
#include "core/presentation_timing.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <bit>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using SDL_Window = void;
using SDL_Renderer = void;
using SDL_Texture = void;
using AudioDevice = std::uint32_t;
constexpr std::uint32_t INIT_VIDEO = 0x20, INIT_AUDIO = 0x10;
constexpr std::uint32_t WINDOW_SHOWN = 0x4, WINDOW_RESIZABLE = 0x20;
constexpr std::uint32_t RENDERER_SOFTWARE = 0x1;
constexpr std::uint32_t PIXELFORMAT_RGBA32_LITTLE_ENDIAN = 0x16762004;
constexpr std::uint16_t AUDIO_S16LSB = 0x8010;
constexpr std::uint32_t QUIT = 0x100, KEYDOWN = 0x300;
constexpr int SDLK_ESCAPE = 27, SDLK_SPACE = 32, SDLK_q = 113, SDLK_RIGHT = 1073741903;
static_assert(std::endian::native == std::endian::little,
              "This diagnostic preview currently expects little-endian RGBA and PCM");

// SDL2's AudioSpec public C structure; intentionally no private SDL types.
// ABI verified at runtime by opening a device. Kept outside the core.
struct AudioSpec {
    int freq;
    std::uint16_t format;
    std::uint8_t channels, silence;
    std::uint16_t samples, padding;
    std::uint32_t size;
    void (*callback)(void*, std::uint8_t*, int);
    void* userdata;
};

class SDL final {
public:
    SDL() {
#ifdef _WIN32
        library_ = LoadLibraryA("SDL2.dll");
#elif defined(__APPLE__)
        library_ = dlopen("libSDL2-2.0.0.dylib", RTLD_NOW | RTLD_LOCAL);
        if (!library_) library_ = dlopen("SDL2.framework/SDL2", RTLD_NOW | RTLD_LOCAL);
#else
        library_ = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library_) throw std::runtime_error("SDL2 runtime library not found");
        try {
#define SDL_SYMBOL(name) sym(name, "SDL_" #name)
            SDL_SYMBOL(Init); SDL_SYMBOL(Quit); SDL_SYMBOL(GetError);
            SDL_SYMBOL(CreateWindow); SDL_SYMBOL(DestroyWindow);
            SDL_SYMBOL(CreateRenderer); SDL_SYMBOL(DestroyRenderer);
            SDL_SYMBOL(CreateTexture); SDL_SYMBOL(DestroyTexture);
            SDL_SYMBOL(UpdateTexture); SDL_SYMBOL(RenderClear);
            SDL_SYMBOL(RenderCopy); SDL_SYMBOL(RenderPresent); SDL_SYMBOL(PollEvent);
            SDL_SYMBOL(OpenAudioDevice); SDL_SYMBOL(CloseAudioDevice);
            SDL_SYMBOL(PauseAudioDevice); SDL_SYMBOL(QueueAudio);
            SDL_SYMBOL(GetQueuedAudioSize); SDL_SYMBOL(ClearQueuedAudio);
#undef SDL_SYMBOL
        } catch (...) { unload(); throw; }
    }
    ~SDL() { unload(); }
    SDL(const SDL&) = delete;
    SDL& operator=(const SDL&) = delete;

    int (*Init)(std::uint32_t){};
    void (*Quit)(){};
    const char* (*GetError)(){};
    SDL_Window* (*CreateWindow)(const char*,int,int,int,int,std::uint32_t){};
    void (*DestroyWindow)(SDL_Window*){};
    SDL_Renderer* (*CreateRenderer)(SDL_Window*,int,std::uint32_t){};
    void (*DestroyRenderer)(SDL_Renderer*){};
    SDL_Texture* (*CreateTexture)(SDL_Renderer*,std::uint32_t,int,int,int){};
    void (*DestroyTexture)(SDL_Texture*){};
    int (*UpdateTexture)(SDL_Texture*, const void*, const void*, int){};
    int (*RenderClear)(SDL_Renderer*){};
    int (*RenderCopy)(SDL_Renderer*,SDL_Texture*,const void*,const void*){};
    void (*RenderPresent)(SDL_Renderer*){};
    int (*PollEvent)(void*){};
    AudioDevice (*OpenAudioDevice)(const char*,int,const AudioSpec*,AudioSpec*,int){};
    void (*CloseAudioDevice)(AudioDevice){};
    void (*PauseAudioDevice)(AudioDevice,int){};
    int (*QueueAudio)(AudioDevice,const void*,std::uint32_t){};
    std::uint32_t (*GetQueuedAudioSize)(AudioDevice){};
    void (*ClearQueuedAudio)(AudioDevice){};
private:
#ifdef _WIN32
    HMODULE library_{nullptr};
#else
    void* library_{nullptr};
#endif
    template<class T> void sym(T& target, const char* name) {
#ifdef _WIN32
        const auto p = GetProcAddress(library_, name);
#else
        const auto p = dlsym(library_, name);
#endif
        if (!p) throw std::runtime_error(std::string("SDL2 missing symbol ") + name);
        static_assert(sizeof(target) == sizeof(p));
        std::memcpy(&target, &p, sizeof(p));
    }
    void unload() noexcept {
        if (!library_) return;
#ifdef _WIN32
        FreeLibrary(library_);
#else
        dlclose(library_);
#endif
        library_ = nullptr;
    }
};

void check(bdmv_status status, const char* step) {
    if (status != BDMV_OK) throw std::runtime_error(
        std::string(step) + " failed: status=" + std::to_string(status));
}
struct Engine final {
    bdmv_engine* ptr{};
    Engine() {
        bdmv_engine_config config{sizeof(config), 0, BDMV_VIDEO_SOFTWARE, BDMV_AUDIO_AUTO};
        check(bdmv_engine_create(&config, &ptr), "engine_create");
    }
    ~Engine() { bdmv_engine_destroy(ptr); }
};
struct Session final {
    bdmv_session* ptr{};
    explicit Session(const Engine& e, const char* source) {
        check(bdmv_session_create(e.ptr, &ptr), "session_create");
        try { check(bdmv_session_open_uri(ptr, source), "session_open_uri"); }
        catch (...) { bdmv_session_destroy(ptr); ptr = nullptr; throw; }
    }
    ~Session() { bdmv_session_destroy(ptr); }
};
struct Frame final { bdmv_decoded_frame_info info{}; std::vector<std::uint8_t> bytes; };
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "Usage: bdmv_sdl_preview <local-file|test://frames> [--max-seconds N]\n"
                     "Space=pause/resume, Right=seek +5s, Q/Escape=quit.\n";
        return 2;
    }
    int max_seconds = 0;
    if (argc == 4) {
        if (std::strcmp(argv[2], "--max-seconds") != 0) return 2;
        try { max_seconds = std::stoi(argv[3]); } catch (...) { return 2; }
        if (max_seconds < 1 || max_seconds > 3600) return 2;
    }
    try {
        SDL s;
        if (s.Init(INIT_VIDEO | INIT_AUDIO) != 0)
            throw std::runtime_error(std::string("SDL_Init: ") + s.GetError());
        struct SDLQuit final { SDL& s; ~SDLQuit() { s.Quit(); } } quit{s};
        SDL_Window* window = s.CreateWindow("BDMV Player - M1d preview", 0, 0, 960, 540,
                                            WINDOW_SHOWN | WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(std::string("SDL_CreateWindow: ") + s.GetError());
        struct WindowCleanup final { SDL& s; SDL_Window* p; ~WindowCleanup() { s.DestroyWindow(p); } } wc{s, window};
        SDL_Renderer* renderer = s.CreateRenderer(window, -1, RENDERER_SOFTWARE);
        if (!renderer) throw std::runtime_error(std::string("SDL_CreateRenderer: ") + s.GetError());
        struct RendererCleanup final { SDL& s; SDL_Renderer* p; ~RendererCleanup() { s.DestroyRenderer(p); } } rc{s, renderer};
        SDL_Texture* texture = nullptr;
        std::uint32_t video_width = 0, video_height = 0;
        struct TextureCleanup final { SDL& s; SDL_Texture*& p; ~TextureCleanup() { if(p) s.DestroyTexture(p); } } tc{s, texture};
        AudioSpec wanted{};
        wanted.freq = 48000; wanted.format = AUDIO_S16LSB;
        wanted.channels = 2; wanted.samples = 1024;
        AudioSpec obtained{};
        AudioDevice audio = s.OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        if (!audio) std::cerr << "warning: audio device unavailable: " << s.GetError() << '\n';
        else if (obtained.freq != 48000 || obtained.channels != 2 || obtained.format != AUDIO_S16LSB) {
            s.CloseAudioDevice(audio); audio = 0;
            std::cerr << "warning: required S16LE stereo 48k device unavailable\n";
        }
        struct AudioCleanup final { SDL& s; AudioDevice& p; ~AudioCleanup() { if(p) s.CloseAudioDevice(p); } } ac{s,audio};

        Engine engine;
        Session session(engine, argv[1]);
        check(bdmv_session_play(session.ptr), "session_play");
        if (audio) s.PauseAudioDevice(audio, 0);
        const auto start = Clock::now();
        std::int64_t clock_anchor_wall = now_ns(), clock_anchor_pts = 0;
        std::int64_t paused_position = 0;
        bool clock_started = false, paused = false, eof = false, done = false;
        std::uint64_t presented = 0, dropped = 0, queued_pcm_bytes = 0;
        std::deque<Frame> video;
        std::int64_t last_audio_end = -1;
        while (!done) {
            alignas(16) std::array<std::uint8_t, 64> event{};
            while (s.PollEvent(event.data())) {
                std::uint32_t type{};
                std::memcpy(&type, event.data(), sizeof(type));
                if (type == QUIT) done = true;
                if (type != KEYDOWN) continue;
                // SDL_KeyboardEvent.keysym.sym is at byte offset 20 in SDL2 ABI.
                std::int32_t key{};
                std::memcpy(&key, event.data() + 20, sizeof(key));
                if (key == SDLK_ESCAPE || key == SDLK_q) done = true;
                else if (key == SDLK_SPACE) {
                    if (!paused) {
                        check(bdmv_session_pause(session.ptr), "pause");
                        paused_position = clock_anchor_pts + (clock_started ? now_ns() - clock_anchor_wall : 0);
                        if (audio) s.PauseAudioDevice(audio, 1);
                    } else {
                        check(bdmv_session_play(session.ptr), "resume");
                        clock_anchor_pts = paused_position;
                        clock_anchor_wall = now_ns();
                        if (audio) s.PauseAudioDevice(audio, 0);
                    }
                    paused = !paused;
                } else if (key == SDLK_RIGHT) {
                    const auto clock_pos = paused ? paused_position :
                        clock_anchor_pts + (clock_started ? now_ns() - clock_anchor_wall : 0);
                    const auto seek_to = clock_pos + 5'000'000'000LL;
                    check(bdmv_session_seek(session.ptr, seek_to), "seek");
                    if (audio) s.ClearQueuedAudio(audio);
                    video.clear();
                    eof = false;
                    clock_anchor_pts = paused_position = seek_to;
                    clock_anchor_wall = now_ns();
                    clock_started = true;
                    last_audio_end = -1;
                }
            }
            if (max_seconds && Clock::now() - start > std::chrono::seconds(max_seconds)) break;
            if (!paused && !eof) {
                // Bounded consumer staging; pushback is held in MediaRuntime's bounded queue.
                // Never let SDL's own audio queue grow without limit either.
                for (int n = 0; n < 24 && video.size() < 8; ++n) {
                    if (audio && s.GetQueuedAudioSize(audio) > 48'000U * 4U / 4U) break;
                    Frame frame;
                    frame.info.struct_size = sizeof(bdmv_decoded_frame_info);
                    auto status = bdmv_session_read_frame(session.ptr, &frame.info, nullptr, 0);
                    if (status == BDMV_AGAIN) break;
                    if (status == BDMV_END_OF_STREAM) { eof = true; break; }
                    if (status != BDMV_BUFFER_TOO_SMALL) check(status, "read_frame query");
                    if (frame.info.required_bytes > 128ULL * 1024 * 1024)
                        throw std::runtime_error("frame buffer exceeds preview limit");
                    frame.bytes.resize(static_cast<std::size_t>(frame.info.required_bytes));
                    check(bdmv_session_read_frame(session.ptr, &frame.info,
                                                  frame.bytes.data(), frame.bytes.size()), "read_frame data");
                    if (!clock_started) {
                        clock_anchor_pts = std::max<std::int64_t>(0, frame.info.pts_ns);
                        clock_anchor_wall = now_ns();
                        clock_started = true;
                    }
                    if (frame.info.kind == BDMV_DECODED_RGBA8) video.push_back(std::move(frame));
                    else if (frame.info.kind == BDMV_DECODED_PCM_S16_STEREO && audio) {
                        if (frame.bytes.size() > std::numeric_limits<std::uint32_t>::max())
                            throw std::runtime_error("audio frame too large");
                        if (s.QueueAudio(audio, frame.bytes.data(),
                                         static_cast<std::uint32_t>(frame.bytes.size())) != 0)
                            throw std::runtime_error(std::string("SDL_QueueAudio: ") + s.GetError());
                        queued_pcm_bytes += frame.bytes.size();
                        last_audio_end = frame.info.pts_ns + frame.info.duration_ns;
                    }
                }
            }
            if (!paused && !video.empty()) {
                const auto now = now_ns();
                const auto media = clock_anchor_pts + (clock_started ? now - clock_anchor_wall : 0);
                auto decision = bdmv::core::VideoTiming::decide(video.front().info.pts_ns, media);
                if (decision.action != bdmv::core::VideoDisposition::Wait) {
                    auto f = std::move(video.front()); video.pop_front();
                    if (decision.action == bdmv::core::VideoDisposition::Drop) ++dropped;
                    else {
                        if (f.info.width != video_width || f.info.height != video_height) {
                            if (texture) s.DestroyTexture(texture);
                            video_width = f.info.width; video_height = f.info.height;
                            texture = s.CreateTexture(renderer, PIXELFORMAT_RGBA32_LITTLE_ENDIAN,
                                                      1, static_cast<int>(video_width),
                                                      static_cast<int>(video_height));
                            if (!texture) throw std::runtime_error(std::string("SDL_CreateTexture: ") + s.GetError());
                        }
                        if (s.UpdateTexture(texture, nullptr, f.bytes.data(),
                                            static_cast<int>(f.info.width * 4)) != 0)
                            throw std::runtime_error(std::string("SDL_UpdateTexture: ") + s.GetError());
                        (void)s.RenderClear(renderer);
                        if (s.RenderCopy(renderer, texture, nullptr, nullptr) != 0)
                            throw std::runtime_error(std::string("SDL_RenderCopy: ") + s.GetError());
                        s.RenderPresent(renderer);
                        ++presented;
                    }
                }
            }
            if (eof && video.empty() && (!audio || s.GetQueuedAudioSize(audio) == 0)) done = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::cout << "video_presented=" << presented << " video_dropped=" << dropped
                  << " queued_pcm_bytes=" << queued_pcm_bytes << " eof=" << eof << '\n';
        if (audio && last_audio_end >= 0) {
            std::cout << "estimated_audio_playhead_ns=" <<
                bdmv::core::estimated_audio_playhead_ns(last_audio_end,
                    s.GetQueuedAudioSize(audio), 48000, 2, 2) << '\n';
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "preview error: " << e.what() << '\n';
        return 1;
    }
}
