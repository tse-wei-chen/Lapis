# Lapis desktop host — Avalonia UI

`Lapis.Desktop.Avalonia` is the product frontend. The former WinForms
application and its test project have been retired. `Lapis.Desktop.Hosting`
remains UI-neutral and orders actor commands together with surface binding.

## Requirements

Windows 10/11, .NET 10 SDK, Visual Studio C++ toolchain, Windows SDK,
CMake, project-pinned vcpkg and D3D11 (or WARP).

## Build and run

```powershell
cmake -S native/MediaRuntime -B build/native -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_OVERLAY_TRIPLETS=$PWD/native/MediaRuntime/vcpkg-triplets" `
  "-DVCPKG_TARGET_TRIPLET=x64-windows-release"
cmake --build build/native --config Release
ctest --test-dir build/native -C Release --output-on-failure
dotnet restore Lapis.slnx
dotnet build Lapis.slnx -c Release --no-restore
dotnet test Lapis.slnx -c Release --no-build
dotnet run --project src/Lapis.Desktop.Avalonia -c Release --no-build
```

The Avalonia app copies `build/native/Release/Lapis.MediaRuntime.dll`
when available. The UI may open without native DLLs; media playback
requires the native build and its dependencies. BDMV requires an
additional libbluray-enabled native build.

## Ownership and user-facing scope

```text
Avalonia MainWindow / PlayerViewModel
  -> IPlayerPlaybackService
    -> NativePlayerPlaybackService (Windows adapter)
      -> DesktopPlaybackHost (serialized command/surface control)
        -> PlaybackSession -> C ABI -> MediaRuntime
NativeControlHost -> proxy HWND -> persistent D3D11 child HWND
```

Avalonia owns the HWND lifetime, not FFmpeg/decoding. The video HWND
must be parked safely across visual detach/attach; native rendering must
stop and release it before destruction. Do not synchronously await
render-worker acknowledgements on the Avalonia dispatcher.

Current UI: open a local file, choose an unprotected BDMV folder, play,
pause/replay, stop and seek ±10 seconds. The former UI's fullscreen,
theater mode, draggable timeline, keyboard shortcuts and diagnostic
export are **not** yet implemented in the Avalonia frontend.

NativeControlHost has HWND airspace limitations. AI subtitles, BD-J
menus and transparent overlays require future GPU compositor/shared
surface work; they must not be painted as ordinary transparent Avalonia
controls above the native video rectangle.

See [Avalonia desktop](avalonia-desktop.md),
[Windows acceptance](desktop-acceptance.md), and
[ADR 0040](../adr/0040-avalonia-desktop-ui.md).
