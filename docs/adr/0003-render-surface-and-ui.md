# ADR-0003 — Video Surface、GPU Lifetime 與 UI 疊圖

- Status: Accepted in principle, integration spike required before M2.
- Input: `VideoFrame {PTS, Epoch, dimensions, color metadata, ref-counted surface lease}`。資料不是 CPU pixels 的同義詞。
- Shared domain: renderer 對上只暴露 `ViewportToken / IVideoPresenter / OutputResult`。D3D11 texture、VkImage、DMA-BUF FD 和 Wayland surface 都不得進 C# domain、公共 C ABI 的 frame DTO。
- Synchronization: producer fence / consumer fence / released surface callbacks、device/context identity、zero-copy capability & conversion cost 是未来的 platform-level descriptor，不承諾所有裝置零拷貝。
- Composition: 字幕、BD-J、AI、OSD ideally 在同一 compositor/output plane；Avalonia native control hosting 的 z-order/透明度限制使跨窗口 overlay 不可靠。必須先測 x64 Win32 + Linux X11 + Wayland/Gamescope、HDR/pass-through 才確立 final viewport contract。
- Fallback: CPU upload or software presentation、HDR→SDR tonemap、device-lost resource rebuild，不要讓 UI 截斷 native 影音生命週期。
- libplacebo: 候選色彩/HDR/渲染底層，須做授權、API compatibility、swapchain integration 試驗；不是現在硬綁的 Core 依賴。
