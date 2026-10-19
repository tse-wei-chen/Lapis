# Avalonia library-first cinema desktop — Windows first

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

The default project stages **all** `build/native/Release/*.dll` alongside
Avalonia, after Native Release is built. To play BDMV directories, use
`scripts/build-bdmv-player.ps1` to build `build/native-bdmv` with libbluray
enabled and publish an isolated `build/Lapis.BdmvPlayer` directory.
The `LapisNativeRuntimeDir` MSBuild property selects the specific Native
build; without it, the standard file-only runtime is selected. The directory contains
`Lapis.MediaRuntime.dll` **and** the FFmpeg shared libraries (and libbluray
dependencies if built with the opt-in feature). No machine-wide PATH changes
should be necessary. A managed-only build can still show the library without
Native installed, but watching a movie requires these DLLs.

If you already built the UI before Native, rebuild the UI. Quick local recovery
on Windows from the repository root:

```powershell
cmake --build build/native --config Release
dotnet build src/Lapis.Desktop.Avalonia/Lapis.Desktop.Avalonia.csproj -c Release
# Check both the main DLL and the avformat/avcodec/avutil dependencies:
Get-ChildItem src/Lapis.Desktop.Avalonia/bin/Release/net10.0-windows/*dll |
  Where-Object Name -Match 'Lapis.MediaRuntime|avcodec|avformat|avutil'
dotnet run --project src/Lapis.Desktop.Avalonia -c Release --no-build
```

If `session_open` returns Unsupported for BDMV, check that you launched
the **BDMV-specific published executable**; it is not interchangeable
with the normal file-only build. The extra `MAKEMKV` directory may remain
in the disc root, but doesn't establish whether the BDMV stream is clear.
AACS/BD+ decryption and BD-J menu navigation remain unsupported.

If error `0x8007007E` remains despite app-local dependencies, check for a
missing MSVC x64 runtime or an unresolved transitive DLL with the Windows
Dependencies viewer or `dumpbin /DEPENDENTS`. Never fix this by blindly
copying random DLLs into System32. The product package must independently
carry its redistributable Native dependencies, subject to THIRD_PARTY.md.

Library cards display disc-supplied local JPEG artwork from
`BDMV/META/DL`, preferring `*_L.jpg` and then `*_S.jpg`. The catalog
stores no bitmap bytes or artwork URL. Optional image files are read
off the UI thread with a bounded decode worker pool and decoded to at most
400 pixels wide; bad/missing/unmounted art falls back to the "L" poster
placeholder. Backup and `MAKEMKV` trees are excluded from artwork
discovery. Tiles own and dispose their decoded bitmaps; no poster loads
touch the media runtime or playback graph.

The app now starts on the **library**, not on the player surface. Import
one or multiple local video files, or one **entire clear Blu-ray disc root**
(containing BDMV/PLAYLIST and BDMV/STREAM). Both the disc root and its BDMV
child normalize to **one** library entry; individual .m2ts clips are not
implicitly cataloged as the movie. Import does not copy media or start
Native MediaRuntime. The durable versioned catalog currently lives under
`%LOCALAPPDATA%/Lapis/Library/catalog.v1.json` behind `IMediaLibrary` and
is replaced atomically. An inaccessible original path is visible in the
library and cannot be played until it becomes available again.

Selecting a library card enters the separate **Watch** page. The same Native
actor opens the typed locator and keeps the GPU child HWND lifetime unchanged.
A disc sidebar lists available `.mpls` file **names only**, not selectable
titles, clip orders or BD-J menus. Real Blu-ray navigation must be routed
through the native navigation controller in a later milestone. Returning
to the library closes the player before hiding its view.

A seek timeline is shown under the video whenever a known
positive duration is available; dragging previews without changing playback,
and releasing commits one actor-owned Seek. A seek that crosses a newer
PlaybackEpoch is discarded. At EOF the timeline is disabled, while Replay
remains available. Actual BDMV playback requires the separately validated,
opt-in `LAPIS_ENABLE_LIBBLURAY=ON` native build, and all its runtime DLLs.
BD-J, AACS/BD+, optical drive access, ISO, subtitle overlay and AI
translation are **not implemented** by this shell.

## Safety/ownership boundaries

```text
MainWindow (library-first shell + lifetime)
  -> LibraryViewModel -> IMediaLibrary -> JsonMediaLibrary (LocalAppData)
  -> explicit Watch selection -> PlayerViewModel (UI state; no HWND or D3D)
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
library import/search/filter, typed locator forwarding, one-shot seek scrubbing (including epoch invalidation),
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
