#pragma once

#include "video/video_presenter.hpp"

#include <memory>

namespace lapis::media::x11 {

// Linux X11/XWayland CPU presentation fallback. A separate XCB connection
// belongs entirely to the render thread; no Xlib Display* crosses the ABI.
// Vulkan and native Wayland remain separate future adapters.
class XcbSoftwareVideoPresenter final : public IVideoPresenter {
public:
    XcbSoftwareVideoPresenter();
    ~XcbSoftwareVideoPresenter() override;

    XcbSoftwareVideoPresenter(const XcbSoftwareVideoPresenter&) = delete;
    XcbSoftwareVideoPresenter& operator=(const XcbSoftwareVideoPresenter&) = delete;

    [[nodiscard]] VideoOutputSurfaceApplyStatus set_output_surface(
        const VideoOutputSurfaceDescriptor& surface) override;
    [[nodiscard]] VideoPresenterBeginStatus begin(PlaybackEpoch epoch) override;
    [[nodiscard]] VideoPresentStatus present(
        const VideoFrame& frame, const VideoPresentContext& context) override;
    void end() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lapis::media::x11
