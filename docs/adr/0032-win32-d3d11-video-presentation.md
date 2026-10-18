# ADR 0032: Win32 D3D11 video presentation is a Windows-only adapter

- Status: Accepted
- Date: 2026-10-18

## Context

The execution graph decodes and schedules video frames and exposes an acknowledged output-surface binding through C ABI v2 and the managed presentation port (ADRs 0030–0031). The Windows composition root still instantiates a headless presenter, so a valid HWND binding cannot actually display a frame.

The existing FFmpeg video processor converts decoded CPU frames to BGRA8 system memory. The first real display backend can consume this result without changing packet routing, decoder types, scheduling, audio, timestamps, or the C ABI.

## Decision

1. Add `video/d3d11/D3D11VideoPresenter` as a Windows-only `IVideoPresenter` implementation using PImpl. D3D11, DXGI, D3DCompiler, WRL/COM, and HWND headers are private to the adapter; core interfaces remain platform-neutral.
2. Compose the D3D11 presenter on Windows only. Other platforms keep the headless presenter, and an unbound Windows session behaves headlessly until a UI host binds its window.
3. Create D3D11 hardware devices first, falling back to WARP if hardware creation fails (important for remote desktops and CI). Build an `IDXGISwapChain1` flip-discard BGRA8 double-buffer swapchain with `CreateSwapChainForHwnd`.
4. Accept BGRA8 `ISystemMemoryVideoSurface` frames only for this first slice. Upload into a dynamically mapped D3D11 shader-resource texture, respecting positive source row stride. Shader code draws a vertexless full-screen triangle, sampling bilinearly into an aspect-preserving viewport on a black back buffer.
5. Query the window client area on each frame and resize the swapchain buffer only when dimensions change. Zero-size/minimized windows skip GPU presentation without faulting or performing a zero-dimension resize.
6. Bind, render, resize, and release the graphics resources only on the render worker's thread. A replacement or detach clears the graphics context, flushes pending commands, and releases the old swapchain/device before the worker acknowledges success. `end()` also releases resources.
7. Translate DXGI device removed/reset/hung failures into `VideoPresentStatus::DeviceLost` so the existing render worker's failure state carries the error. Other rendering failures remain `Failed`; unsupported frame formats remain `UnsupportedSurface`.
8. Add Windows presenter tests covering BGRA8 drawing, non-BGRA rejection, invalid HWND, resize, live replacement, detach, and unbound headless behavior. Extend the public C ABI smoke test to bind a real host HWND and verify that playback completes without a presenter failure.
9. Keep window ownership with the caller: Lapis borrows the HWND, never destroys it, and a host must wait for an acknowledged detach/replacement or a session close before destroying the previous HWND.

## Constraints

- This is an initial SDR BGRA8 presentation path. It intentionally uses CPU frame upload, not decoder-native D3D textures, zero-copy composition, or a GPU color-conversion graph.
- Pixel format conversion remains in the video-processing stage. The presenter only uploads BGRA, scales and composites to the swapchain; it does not perform YUV-to-RGB conversion.
- Runtime shader compilation is isolated to the D3D11 adapter. Precompiled shader distribution and diagnostics can be added without touching any other module.
- Output device-loss recovery is reported rather than automatically restarted. Automatic recovery and window-resize events independent of a new frame will be separate changes.
- Color metadata, BT.2020/PQ/HLG transforms, HDR10, DPI-aware rendering policy, exclusive/fullscreen, subtitle overlays, GPU-decode texture ingestion, and deeper frame pacing remain separate milestones.
