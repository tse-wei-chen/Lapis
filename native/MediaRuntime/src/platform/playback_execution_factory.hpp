#pragma once

#include <memory>

namespace lapis::media {
class DiagnosticEventJournal;
class IVideoPresenter;
class PlaybackExecution;
class PlaybackMediaSession;

namespace platform {

// Platform composition root: the portable session, scheduler and decoder never
// select D3D11, WASAPI, Vulkan, PipeWire or window-system APIs themselves.
// Linux selects an X11/XWayland CPU fallback when a valid X11 window is
// explicitly bound; otherwise playback remains headless. Vulkan, Wayland and
// PipeWire are independent follow-up adapters.
[[nodiscard]] std::unique_ptr<IVideoPresenter> create_video_presenter();

[[nodiscard]] std::unique_ptr<PlaybackExecution> create_execution(
    PlaybackMediaSession& media,
    std::shared_ptr<DiagnosticEventJournal> journal);

} // namespace platform
} // namespace lapis::media
