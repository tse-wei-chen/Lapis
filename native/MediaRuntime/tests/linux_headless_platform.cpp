#include "platform/playback_execution_factory.hpp"

#include "core/playback_epoch.hpp"
#include "video/video_output_surface.hpp"
#include "video/video_presenter.hpp"

int main() {
    using namespace lapis::media;
    auto presenter = platform::create_video_presenter();
    if (!presenter) return 1;

    // Unbound render remains headless; Win32 must be rejected on Linux.
    // The X11 output path is exercised in a separate Xvfb pixel test.
    if (presenter->set_output_surface({VideoOutputSurfaceKind::None, 0})
        != VideoOutputSurfaceApplyStatus::Ready) return 2;
    if (presenter->set_output_surface({VideoOutputSurfaceKind::Win32Window, 1})
        != VideoOutputSurfaceApplyStatus::Unsupported) return 3;
    if (presenter->begin(PlaybackEpoch{1})
        != VideoPresenterBeginStatus::Ready) return 4;
    presenter->end();
    return 0;
}
