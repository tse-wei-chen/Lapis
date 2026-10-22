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

## Linux native headless validation

The portable C++20 Core, C ABI, FFmpeg decode pipeline and execution tests
are built and exercised on Ubuntu 24.04 in the `linux_headless` CI job.
This uses system FFmpeg development packages (via pkg-config), not the
Windows vcpkg binary cache. D3D11/WASAPI live in the separate Platform
composition target; Linux currently has an explicitly **headless, silent**
backend. **This is not Linux desktop video or audio playback support.**
See [Linux headless CI and platform-boundary notes](docs/architecture/linux-headless-ci.md).

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

## Local movie artwork (offline)

Lapis never guesses titles or downloads third-party posters automatically. To show
a cover for a local video, place an image alongside the media file; for example:

```text
Movies/
  Example.mkv
  Example.poster.jpg   # preferred
  Example.jpg          # also supported
```

Supported local formats are JPEG and PNG, with a maximum source size of 16 MiB.
The lookup is case-insensitive, supports `<title>.poster`, `<title>.cover`,
and `<title>` names, and also allows a generic `poster.jpg`, `cover.jpg`,
or `folder.png` when the folder contains only one movie. This deliberately
avoids assigning the same poster to several unrelated titles in a flat folder.
Blu-ray artwork continues to be read from `BDMV/META/DL` using the disc's
bundled large JPEG. Images are discovered and decoded in bounded background
workers; no artwork is required for playback. If you add, replace, or
delete a local cover after opening Lapis, open the movie details and click
**重新掃描封面**. This rechecks sidecars immediately without reimporting the
movie, touching the catalog, or restarting Native playback.

## My Favorites

In Movie Details, choose **☆ 加入我的最愛** to pin a local movie or Blu-ray
disc to the **我的最愛** shelf and category. The pin is reversible, independent
of viewing history, and never moves or modifies the media itself. Lapis stores
favorites in a versioned, atomic sidecar beside the catalog under the user's
local application data; corrupted or unsupported versions are preserved rather
than silently reset. Missing/offline media remains pinned until the user
explicitly removes it from Favorites.

## Repair a moved or offline library source

If a video moves to a different folder or a Blu-ray drive is remounted at
another location, the original library entry is retained. Open that movie's
details and use **修復來源位置…** to select the **same** local video file or
complete BDMV disc root at its new location. Lapis validates the replacement
before atomically updating the catalog. The movie ID, display title, original
import date, favorites and watch history stay intact; no media bytes are
copied or renamed. Only missing/offline items may be relinked: pointing a
valid entry at a different source or duplicating another movie's catalog
location is rejected. Relinking does not establish that two different video
files contain identical content; the user must select the original movie.

## Customize movie titles

Open a film in the cinema library, choose **編輯片名**, enter a custom title,
and save. Lapis updates the title in the local catalog atomically; the actual
video file or Blu-ray folder is never renamed or moved. Existing movie IDs,
favorites, watch history, resume bookmarks and local cover sidecars remain
attached to the same media. Search, title sorting and the home display update
after saving. Titles are limited to 160 characters and cannot include control
characters; cancel leaves the library unchanged.

## Remove a movie from the local catalog

In the movie detail sheet, select **從收藏庫移除…** and explicitly confirm.
Cancel returns to the movie without changing anything. Confirm removes only the
catalog entry and its associated local favorites, watch-history and resume
bookmarks. **Lapis never deletes, modifies, decrypts or moves the original
video file or Blu-ray folder.** The secondary management screen's Remove action
uses the same cleanup logic. If a sidecar cleanup fails after catalog removal,
Lapis reports the partial failure instead of pretending that all private
metadata was erased.

## Durable playback snapshots

Lapis saves each eligible watch snapshot to the atomic **v2** local journal
(`catalog.v1.json.watch.v2.json`). Playback timer callbacks and final close
saves can race: the journal compares `UpdatedAtUtc` while holding its write
gate, rejecting observations older than or equal to the latest persisted one.
The single `SaveAsync` API reports whether a snapshot was accepted. Only
accepted saves are applied to the in-memory Continue Watching and History
shelves, where the same timestamp guard prevents late UI continuations from
rewinding state. A genuinely later viewing session can resume normally.

Lapis has **not released yet**: the unreleased v1 watch format is intentionally
unsupported, with no migration or API compatibility shim. Incompatible files
fail closed without overwriting them. Original video and Blu-ray media, and
the Native playback ABI, are unchanged.

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
