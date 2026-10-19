# ADR 0040: Avalonia as the product desktop UI, WinForms as a test harness

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
3. `WindowsVideoSurface : NativeControlHost` owns an independent D3D11
   child HWND and a hidden parking window. Avalonia visual-root transitions
   park and reattach the child synchronously in Win32, **without blocking the
   dispatcher on a render ACK**. Final shutdown calls
   `DesktopPlaybackHost.DisposeAsync()` and waits for native worker teardown
   **before** destroying the child HWND. No Avalonia composition object is
   lent to the native renderer.
4. Keep playback commands and snapshots UI-neutral. Coalesce high-frequency
   observations so at most one dispatcher refresh is pending; read current
   snapshot rather than replaying stale captured state after Seek.
5. Place all interaction chrome outside the HWND video rectangle. Avalonia's
   NativeControlHost has **airspace/Z-order limitations**, so pretending
   transparent native subtitles/BD-J menus already overlay correctly would
   be unsafe. A future GPU compositor or properly shared rendering surface
   must own video/subtitles/menus/AI text ordering.
6. WinForms remains in the solution as a legacy/regression harness only.
   No new product controls should be developed there. Retain its existing
   Windows GPU acceptance tests until equivalent Avalonia HW lifecycle and
   DPI/device-loss gates exist.
7. Target `net10.0-windows` for the first runnable app. Avalonia's View and
   ViewModel abstractions are portable, but the current D3D11/Win32 playback
   adapter cannot run on macOS/Linux; never claim cross-platform decoding.

## Verification and open gates

- Windows managed build/test must restore the pinned Avalonia dependencies,
  compile XAML and cover BDMV locator validation and ViewModel playback
  state/error handling without loading native DLLs.
- Existing Native/WinForms GPU tests remain compatibility gates.
- **Open:** actual Avalonia + D3D11 first-frame acceptance, persistent HWND
  across Avalonia visual-root transitions, 100/150/200% DPI, close under
  GPU load, multi-monitor, suspend/resume, device removal, and subtitle/menu
  compositor. Do not merge as release-ready until those are verified.

## Architectural consequence

A future non-Windows backend plugs into the `IPlayerPlaybackService` and
an OS-specific video surface, not a new PlaybackSession actor or decoder.
The shared media platform and its stable C ABI remain the source of truth.
