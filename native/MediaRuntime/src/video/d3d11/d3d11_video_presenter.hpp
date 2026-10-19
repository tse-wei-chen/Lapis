#pragma once

#include "video/video_presenter.hpp"

#include <memory>

namespace lapis::media::d3d11 {

// Windows-only presentation adapter. All graphics state is render-thread-owned;
// no D3D, DXGI, HWND, or COM type crosses the IVideoPresenter boundary.
class D3D11VideoPresenter final : public IVideoPresenter {
public:
    D3D11VideoPresenter();
    ~D3D11VideoPresenter() override;

    D3D11VideoPresenter(const D3D11VideoPresenter&) = delete;
    D3D11VideoPresenter& operator=(const D3D11VideoPresenter&) = delete;

    [[nodiscard]] VideoOutputSurfaceApplyStatus set_output_surface(
        const VideoOutputSurfaceDescriptor& surface) override;
    [[nodiscard]] VideoPresenterBeginStatus begin(PlaybackEpoch epoch) override;
    [[nodiscard]] VideoPresentStatus present(
        const VideoFrame& frame,
        const VideoPresentContext& context) override;
    [[nodiscard]] VideoPresenterDiagnostics diagnostics() const noexcept override;
    void set_diagnostics_journal(
        std::shared_ptr<DiagnosticEventJournal> journal) noexcept override;
    void end() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lapis::media::d3d11
