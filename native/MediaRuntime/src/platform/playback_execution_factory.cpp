#include "platform/playback_execution_factory.hpp"

#include "execution/playback_execution.hpp"
#include "process/ffmpeg/ffmpeg_video_frame_converter.hpp"
#include "session/playback_media_session.hpp"
#include "video/video_presenter.hpp"

#if defined(_WIN32)
#include "audio/wasapi/wasapi_shared_audio_output_provider.hpp"
#include "process/ffmpeg/ffmpeg_audio_frame_converter.hpp"
#include "video/d3d11/d3d11_video_presenter.hpp"
#elif defined(__linux__)
#include "audio/alsa/alsa_pcm_audio_output.hpp"
#include "process/ffmpeg/ffmpeg_audio_frame_converter.hpp"
#include "video/x11/xcb_software_video_presenter.hpp"
#endif

#include <memory>
#include <utility>

namespace lapis::media::platform {
namespace {

#if !defined(_WIN32) && !defined(__linux__)
// Explicit non-rendering fallback on platforms without an output adapter.
// Never advertise this as a desktop video output implementation.
class HeadlessVideoPresenter final : public IVideoPresenter {
public:
    [[nodiscard]] VideoPresenterBeginStatus begin(PlaybackEpoch) override {
        return VideoPresenterBeginStatus::Ready;
    }

    [[nodiscard]] VideoPresentStatus present(
        const VideoFrame&, const VideoPresentContext&) override {
        return VideoPresentStatus::Presented;
    }

    void end() noexcept override {}
};
#endif

} // namespace

std::unique_ptr<IVideoPresenter> create_video_presenter() {
#if defined(_WIN32)
    return std::make_unique<d3d11::D3D11VideoPresenter>();
#elif defined(__linux__)
    // X11/XWayland CPU fallback. No DISPLAY or unbound = headless.
    return std::make_unique<x11::XcbSoftwareVideoPresenter>();
#else
    return std::make_unique<HeadlessVideoPresenter>();
#endif
}

std::unique_ptr<PlaybackExecution> create_execution(
    PlaybackMediaSession& media,
    std::shared_ptr<DiagnosticEventJournal> journal) {
    auto video_processor = std::make_unique<
        ffmpeg::FfmpegVideoFrameConverter>();
    auto presenter = create_video_presenter();

#if defined(_WIN32)
    auto audio_provider = std::make_unique<
        wasapi::WasapiSharedAudioOutputProvider>();
    AudioFrameProcessorFactory audio_processor_factory =
        [](const AudioFormat& target_format)
            -> std::unique_ptr<IAudioFrameProcessor> {
            return std::make_unique<
                ffmpeg::FfmpegAudioFrameConverter>(target_format);
        };
    return std::make_unique<PlaybackExecution>(
        media, std::move(video_processor), std::move(presenter),
        std::move(audio_provider), std::move(audio_processor_factory),
        PlaybackExecutionConfig{}, std::move(journal));
#elif defined(__linux__)
    // Linux ALSA PCM can route through pipewire-alsa when installed. Format
    // conversion remains in the portable FFmpeg processing adapter.
    auto audio_provider = std::make_unique<
        alsa::AlsaPcmAudioOutputProvider>();
    AudioFrameProcessorFactory audio_processor_factory =
        [](const AudioFormat& target_format)
            -> std::unique_ptr<IAudioFrameProcessor> {
            return std::make_unique<
                ffmpeg::FfmpegAudioFrameConverter>(target_format);
        };
    return std::make_unique<PlaybackExecution>(
        media, std::move(video_processor), std::move(presenter),
        std::move(audio_provider), std::move(audio_processor_factory),
        PlaybackExecutionConfig{}, std::move(journal));
#else
    return std::make_unique<PlaybackExecution>(
        media, std::move(video_processor), std::move(presenter),
        PlaybackExecutionConfig{}, std::move(journal));
#endif
}

} // namespace lapis::media::platform
