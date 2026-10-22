#include "platform/playback_execution_factory.hpp"

#include "core/playback_epoch.hpp"
#include "video/video_output_surface.hpp"
#include "video/video_presenter.hpp"

int main() {
    using namespace lapis::media;
    auto presenter = platform::create_video_presenter();
    if (!presenter) return 1;

    // Headless is never a replacement for a real Vulkan/X11/Wayland backend.
    if (presenter->set_output_surface({VideoOutputSurfaceKind::None, 0})
        != VideoOutputSurfaceApplyStatus::Ready) return 2;
    if (presenter->set_output_surface({VideoOutputSurfaceKind::Win32Window, 1})
        != VideoOutputSurfaceApplyStatus::Unsupported) return 3;
    if (presenter->begin(PlaybackEpoch{1})
        != VideoPresenterBeginStatus::Ready) return 4;
    presenter->end();
    return 0;
}
