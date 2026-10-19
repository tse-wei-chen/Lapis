# ADR 0040: Avalonia as the sole product desktop UI

- Status: Accepted (first Windows vertical slice)
- Date: 2026-10-19

## Context

The original architecture describes `Player.App` as a replaceable C# MVVM
control plane above the UI-neutral playback actor. WinForms was a fast
D3D11/FFmpeg validation shell, but continuing to add product features there
would create a migration burden. The owner explicitly selected **Avalonia**
over WinUI 3 for the product UI. Cross-platform presentation architecture
is desirable; the current media renderer itself is still Windows/D3D11.

## Decision

1. Add `Lapis.Desktop.Avalonia` with Avalonia 11.3.22, XAML/Fluent-themed
   views and a testable `PlayerViewModel`. UI state/commands depend on an
   `IPlayerPlaybackService` port, not the C ABI, FFmpeg or HWND handles.
2. The first `NativePlayerPlaybackService` is a Windows composition adapter.
   It creates the existing `NativePlaybackRuntime` only when media is opened
   (so the app can display a useful missing-DLL error), and delegates to the
   UI-neutral `DesktopPlaybackHost` for Open/Play/Pause/Seek/Close.
3. `WindowsVideoSurface : NativeControlHost` owns a persistent D3D11
   child HWND and a hidden independent parking window. Critically,
   `base.CreateNativeControlCore(parent)` creates an **Avalonia-owned proxy
   HWND**, with the GPU child nested below it. Avalonia may reparent/reuse
   this proxy without calling CreateNativeControlCore again. If it defers
   and eventually destroys the proxy, the GPU HWND is parked *before*
   base.DestroyNativeControlCore releases the proxy. The next proxy
   reattaches the same GPU child. No synchronous render ACK is awaited by
   the dispatcher. Final shutdown awaits `DesktopPlaybackHost.DisposeAsync()`
   before destroying the D3D11 child HWND.
4. Keep playback commands and snapshots UI-neutral. Coalesce high-frequency
   observations so at most one dispatcher refresh is pending; read current
   snapshot rather than replaying stale captured state after Seek.
5. Avalonia arranges/resizes the proxy only. `WindowsVideoSurface` must
   explicitly update the nested GPU HWND client size from arranged DIP
   bounds * RenderScaling, triggering native DXGI/resize handling at the
   correct physical dimensions. The Windows GPU acceptance asserts the
   child is larger than 1x1 and tracks an explicit Avalonia host layout resize, regardless of virtual CI screen size.
6. Place all interaction chrome outside the HWND video rectangle. Avalonia's
   NativeControlHost has **airspace/Z-order limitations**, so pretending
   transparent native subtitles/BD-J menus already overlay correctly would
   be unsafe. A future GPU compositor or properly shared rendering surface
   must own video/subtitles/menus/AI text ordering.
7. Remove the previous WinForms shell and test project; Avalonia is
   the sole desktop UI. Retain native Win32 GPU soak/parent-churn tests
   plus real Avalonia desktop acceptance. Physical GPU/DPI/device-loss
   tests remain separate release gates.

8. Target `net10.0-windows` for the first runnable app. Avalonia's View and
   ViewModel abstractions are portable, but the current D3D11/Win32 playback
   adapter cannot run on macOS/Linux; never claim cross-platform decoding.

## Verification and open gates

- Windows managed build/test must restore the pinned Avalonia dependencies,
  compile XAML and cover BDMV locator validation and ViewModel playback
  state/error handling without loading native DLLs.
- Native Win32 GPU soak, parent-churn and injected DeviceLost tests remain additional gates.
- **New CI gate:** a full Avalonia desktop lifetime and D3D11 swap-chain
  acceptance with a real FFmpeg media fixture, six NativeControlHost
  detach/attach cycles, seek/replay and host-before-HWND shutdown ordering.
- **Still open:** pixel-verified first-frame presentation, 100/150/200%
  DPI, physical GPU/drivers, multi-monitor, suspend/resume, device removal,
  multi-hour stability and a subtitle/menu GPU compositor. Not release-ready.

## Architectural consequence

A future non-Windows backend plugs into the `IPlayerPlaybackService` and
an OS-specific video surface, not a new PlaybackSession actor or decoder.
The shared media platform and its stable C ABI remain the source of truth.
