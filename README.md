# Lapis

Lapis is a Windows-first media platform for high-quality local and Blu-ray playback, with non-blocking AI transcription and translation designed as first-class capabilities rather than playback dependencies.

## Architecture baseline

Lapis separates the **control plane** from the **media data plane**:

- `src/Lapis.Contracts` contains stable product/domain contracts only.
- `src/Lapis.Application` owns product orchestration, serialized playback commands, state snapshots, epochs, and user-facing playback policy.
- `src/Lapis.Library` owns independent source-catalog contracts, one-item-per-disc normalization, and persistent local indexes; it never decodes media.
- `src/Lapis.Desktop.Hosting` owns UI-neutral host command/surface ordering; `src/Lapis.Desktop.Avalonia` is the **product desktop UI** with MVVM and a Windows-specific D3D11 HWND adapter. the retired WinForms shell and test project are removed.
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

Build the native runtime and managed solution, then launch the **Avalonia** product UI:

```powershell
# Configure the native build with the project-pinned vcpkg manifest/triplet first.
cmake --build build/native --config Release
dotnet build Lapis.slnx -c Release
dotnet run --project src/Lapis.Desktop.Avalonia -c Release --no-build
```

The standard Avalonia build stages `build/native/Release/*.dll` for file playback.
For a **BDMV-capable player**, use the isolated opt-in build instead:

```powershell
.\scripts\build-bdmv-player.ps1
.\build\Lapis.BdmvPlayer\Lapis.Desktop.Avalonia.exe
```

The script uses `VCPKG_ROOT`, then `vcpkg` on `PATH`, then the vcpkg root
recorded in `build/native/CMakeCache.txt`. Use `-VcpkgRoot C:\vcpkg` if none
of those locations apply.
The script enables libbluray with a separate `build/native-bdmv` tree and
publishes its Native and transitive DLLs together. vcpkg's installed packages
are kept under the current user's local application data directory so MSYS
dependency builds do not inherit a non-ASCII workspace path. Standard file
playback builds are not overwritten. Manual `dotnet publish` can select the variant
via `-p:LapisNativeRuntimeDir=<absolute native Release path>`. It opens on a cinema **library home**; users import local videos or an entire BDMV disc root without copying bytes, then select a library card to enter a distinct Watch page with Play/Pause, Stop and seek. A disc sidebar previews MPLS filenames without claiming supported playlist selection or BD-J navigation. The renderer still uses a D3D11 child HWND hosted by Avalonia NativeControlHost: no transparent Avalonia subtitle or BD-J overlay is supported yet. BDMV requires the separate opt-in libbluray Native build; HDR, AI subtitles, packaging and real-disc hardware acceptance remain future milestones. Real Avalonia GPU acceptance and native Win32 soak tests cover HWND lifecycle regression. See [Avalonia desktop development notes](docs/development/avalonia-desktop.md) and [desktop-host notes](docs/development/desktop-host.md).

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
