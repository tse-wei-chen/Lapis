# Lapis

Lapis is a Windows-first media platform for high-quality local and Blu-ray playback, with non-blocking AI transcription and translation designed as first-class capabilities rather than playback dependencies.

## Architecture baseline

Lapis separates the **control plane** from the **media data plane**:

- `src/Lapis.Contracts` contains stable product/domain contracts only.
- `src/Lapis.Application` owns product orchestration, serialized playback commands, state snapshots, epochs, and user-facing playback policy.
- `src/Lapis.Desktop.Hosting` owns UI-neutral host command/surface ordering; `src/Lapis.Desktop.WinForms` is the first runnable Windows desktop shell.
- `native/MediaRuntime` owns the hot media path, the executable immutable media graph, clocks/schedulers, and a stable C ABI.
- third-party media libraries live behind native adapters; their types must not leak into managed contracts or native core contracts.
- AI observes media through taps and must never block playback.

The executable `GraphPlan` has one owner: native `MediaRuntime/Core`. Managed code expresses intent and policy, not decoder/renderer node implementations.

## Build

Managed:

```powershell
dotnet restore Lapis.slnx
dotnet build Lapis.slnx -c Release --no-restore
dotnet test Lapis.slnx -c Release --no-build
```

Native development uses the pinned vcpkg manifest in `native/MediaRuntime`:

```powershell
cmake -S native/MediaRuntime -B build/native -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build/native --config Release
ctest --test-dir build/native -C Release --output-on-failure
```

## Desktop player (Windows)

Build the native runtime and the managed solution, then run the first WinForms shell:

```powershell
# Configure the native build with the project-pinned vcpkg manifest/triplet first.
cmake --build build/native --config Release
dotnet build Lapis.slnx -c Release
dotnet run --project src/Lapis.Desktop.WinForms -c Release --no-build
```

The Desktop project automatically copies `build/native/Release/Lapis.MediaRuntime.dll` into its output when that file exists. It currently opens local video files, renders SDR BGRA8 through D3D11/DXGI to a real HWND, and supports Play/Pause, Stop, seek-to-seconds, and an actual native playback progress bar. The playback actor reconciles real native Ended/Faulted/position via an optional observation port. Blu-ray navigation, HDR, subtitles, AI translation and packaging are separate milestones. See [desktop-host development notes](docs/development/desktop-host.md) for Windows prerequisites and limitations.

## Repository layout

```text
src/
  Lapis.Contracts/
  Lapis.Application/
native/
  MediaRuntime/
    src/core/
    src/source/
    src/demux/
    src/decode/
tests/
  Lapis.Contracts.Tests/
  Lapis.Application.Tests/
  Lapis.Architecture.Tests/
docs/
  architecture/
  adr/
```

Architecture decisions live in `docs/adr`. Dependency rules are executable in `Lapis.Architecture.Tests` and native CTest targets.
