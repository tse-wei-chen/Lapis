# Avalonia desktop player — Windows first

**Product UI:** `src/Lapis.Desktop.Avalonia` (Avalonia 11.3.22 with MVVM).
**Former WinForms shell and tests:** removed.

This is a first vertical slice, not a claim of complete PowerDVD parity.
UI/VM can be ported, but Native FFmpeg/D3D11/WASAPI and the Win32 HWND output
remain Windows-specific.

## Run

Build the Native runtime with CMake/vcpkg per the repository README, then:

```powershell
dotnet restore Lapis.slnx
dotnet build Lapis.slnx -c Release --no-restore
dotnet run --project src/Lapis.Desktop.Avalonia -c Release --no-build
```

The managed project copies `build/native/Release/Lapis.MediaRuntime.dll`
beside its executable when it exists; codec dependencies also need to be
on the DLL search path or deployed next to the app. The window can open
without a Native DLL, but playing media cannot.

Use **開啟影片** for local files and **開啟 BDMV** for an unprotected
disc directory. A seek timeline is shown under the video whenever a known
positive duration is available; dragging previews without changing playback,
and releasing commits one actor-owned Seek. A seek that crosses a newer
PlaybackEpoch is discarded. At EOF the timeline is disabled, while Replay
remains available. Actual BDMV playback requires the separately validated,
opt-in `LAPIS_ENABLE_LIBBLURAY=ON` native build, and all its runtime DLLs.
BD-J, AACS/BD+, optical drive access, ISO, subtitle overlay and AI
translation are **not implemented** by this shell.

## Safety/ownership boundaries

```text
MainWindow (Avalonia picker + lifetime)
  -> PlayerViewModel (UI state, commands; no HWND or D3D)
    -> IPlayerPlaybackService
      -> NativePlayerPlaybackService (Windows composition)
        -> DesktopPlaybackHost
          -> PlaybackSession Actor
            -> Native MediaRuntime
NativeControlHost -> persistent child HWND borrowed by DesktopPlaybackHost
```

The video host uses an **Avalonia-owned proxy HWND** as the immediate
parent of the permanent GPU output HWND. Avalonia can move/park/re-attach
the proxy on fast visual-root detach/attach without calling the control's
CreateNativeControlCore again. If Avalonia later destroys the proxy, the
GPU output HWND moves to an independently owned hidden parking window
before the proxy is destroyed; a new proxy then reclaims it. The GPU HWND
is destroyed **only after** the host's native shutdown/worker join
completes. The child HWND is independently resized during Avalonia layout
arrange, using the top-level RenderScaling to convert DIP to physical
pixels. Without that step, Avalonia resizes only its proxy while the D3D11
output stays at the original 1x1 child-window size. Never wait synchronously
for native render ACK inside Avalonia UI/native host callbacks.

The first real Win32 integration test discovered that parking the GPU child
directly on every Avalonia detach is wrong: Avalonia can reuse its own
existing attachment without re-invoking CreateNativeControlCore, leaving
the GPU HWND stranded in the parking window. Proxy-host ownership prevents
this race; the acceptance now verifies the GPU HWND follows the Avalonia
top-level window across both immediate and deferred detach/attach cycles.

NativeControlHost is a real HWND: Avalonia controls **cannot reliably
overlay it**, including styled translucent subtitle/mouse menus. Put controls
outside the video rectangle until a proper GPU compositor/shared surface
exists. This is explicitly a blocker for final AI subtitles/BD-J UI.

## Tests

`tests/Lapis.Desktop.Avalonia.Tests` covers local BDMV directory matching,
typed locator forwarding, one-shot seek scrubbing (including epoch invalidation),
enabled-state notifications and ViewModel errors without real Native DLLs.
The Native Windows CI plane now runs a **dedicated opt-in REAL Avalonia
lifecycle acceptance**, after building MediaRuntime: it starts a true
Avalonia Win32 desktop lifetime on an STA, opens the same FFmpeg/D3D11
fixture used for native Windows acceptance, waits for the real native
SwapChainCreated diagnostic, detaches/reattaches the NativeControlHost six
times while keeping the same output HWND, verifies physical D3D11-child
resize under an explicit Avalonia control layout change, exercises seek, EOF, replay,
then shuts down the actor before destroying the HWND. It is not a
substitute for visual first-frame pixel verification, DPI 100/150/200%,
physical GPUs, device loss or multi-hour stability.

The integration test is gated by `LAPIS_AVALONIA_NATIVE_ACCEPTANCE=1`
and `LAPIS_NATIVE_UI_MEDIA`, so managed unit CI needs no Native DLL.
Separate native Win32 soak and parent-churn tests provide additional acceptance coverage.
