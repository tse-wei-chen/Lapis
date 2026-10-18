# Running the first Lapis desktop video host

This slice adds a runnable Windows **WinForms shell** with a reusable
UI-framework-neutral desktop playback coordinator. It is a development host, not
a packaged PowerDVD replacement yet.

## Requirements

- Windows 10/11 with the .NET **10** SDK and Windows Desktop runtime
- Visual Studio C++ toolchain, Windows SDK, CMake
- vcpkg using the native project's pinned manifest/triplet
- An accessible D3D11 adapter; WARP software fallback is supported
- A local media file readable by the existing FFmpeg source adapter

## Build and run

From repository root in a Windows PowerShell with vcpkg configured:

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
dotnet run --project src/Lapis.Desktop.WinForms -c Release --no-build
```

The WinForms project copies the built `Lapis.MediaRuntime.dll` from
`build/native/Release` beside its executable **when the file exists**. The
managed solution builds and tests without requiring a native DLL in CI. If native
FFmpeg dependencies are configured as dynamic libraries instead of the pinned
static triplet, those DLLs must also be deployed next to the executable.

## User interaction

Open a local `.mkv`, `.mp4`, `.m2ts`, `.ts`, `.avi`, `.mov`, `.webm`
or similar FFmpeg-supported file through **開啟影片**, Ctrl+O, or drag and drop.
The host opens the media through the playback actor, binds its HWND through
`IVideoOutputSurfaceBinding`, then starts playback. A separate optional
`IPlaybackRuntimeObserver` polls the existing native C ABI snapshot away from
the UI thread, and only matching session/epoch updates reach the actor state.
The progress bar uses native position and open-result duration; releasing its
thumb seeks to a proportional media timestamp. Space toggles pause.
**跳至** seeks to an absolute time in seconds. **停止** closes the media
generation and detaches the output target.

Normal form shutdown cancels native-window destruction until the host has
disposed its session (including render thread + swapchain). This must be
maintained for other desktop frameworks: the UI must not destroy an HWND
while the native presenter is still using it.

## Architectural boundary

```text
WinForms UI / HWND owner
  -> Lapis.Desktop.Hosting (framework-neutral sequencing)
      -> PlaybackSession actor -> IPlaybackRuntime
      -> IVideoOutputSurfaceBinding
          -> Lapis.Runtime.Native -> stable C ABI v2
              -> PlaybackExecution -> D3D11VideoPresenter
```

The desktop shell does **not** call C ABI or DXGI directly. `DesktopPlaybackHost`
takes independently injectable transport and surface-binding ports. It
serializes Open/Bind/Play, seek and output-window replacement; the binding port
waits off the UI thread for the render-thread ACK.

## Important current limitations

- This is local-file playback, **not complete BDMV navigation**. Directory
  playlists/BD-J menus and protected commercial discs are not supported yet.
- The output is SDR BGRA8 CPU-upload; GPU decode, HDR color management,
  subtitle overlay and device-loss recovery are not part of the shell.
- Runtime snapshots are sampled every 150 ms while active, rather than being
  pushed on each frame. The visible position reflects the last observed native
  clock and is not a sample-accurate output-position guarantee. A future event
  channel can replace polling behind the same optional observation port.
- Recreating a WinForms video control's HWND externally (such as a host-specific
  DPI/handle-recreation workflow) requires a window-lifetime transition before
  the old handle is destroyed. The initial shell manages ordinary Stop and Form
  close, but that additional handle-recreation workflow is not implemented.
- On native adapter startup errors, build the native Release target and check
  native DLL compatibility (ABI v2) and dependency availability.
- No installer, signed binaries, or redistributable runtime packaging are
  provided in this development milestone.
