# Preview: unprotected Blu-ray BDMV directory source

Lapis's native media runtime can optionally open an **unprotected** Blu-ray
directory as a virtual MPEG-TS stream through libbluray. This is NOT full
PowerDVD-equivalent disc playback: there is no BD-J, menu rendering, actor-
coordinated title/chapter switching, optical drive access, ISO support, or
AACS/BD+ decryption. The library-first Avalonia app imports one complete disc root (or its BDMV
child) as one collection item. Only a native build with libbluray enabled can
actually open the Directory locator.

## Windows x64: build and publish the BDMV player

With `VCPKG_ROOT` configured, run from the repository root:

```powershell
.\scripts\build-bdmv-player.ps1
.\build\Lapis.BdmvPlayer\Lapis.Desktop.Avalonia.exe
```

Use `-VcpkgRoot C:\vcpkg` if needed. The script explicitly enables both
`VCPKG_MANIFEST_FEATURES=bluray` and `LAPIS_ENABLE_LIBBLURAY=ON`, builds
the adapter, validates its source contract, then publishes all relevant
Native DLLs to an isolated executable folder. No overwriting the ordinary
file-only Native build or system PATH modifications.

Manual equivalent for the Native build only:

```powershell
cmake -S native/MediaRuntime -B build/native-bdmv -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_OVERLAY_TRIPLETS=$PWD/native/MediaRuntime/vcpkg-triplets" `
  "-DVCPKG_TARGET_TRIPLET=x64-windows-release" `
  "-DVCPKG_MANIFEST_FEATURES=bluray" `
  "-DLAPIS_ENABLE_LIBBLURAY=ON"
cmake --build build/native-bdmv --config Release
ctest --test-dir build/native-bdmv -C Release --output-on-failure
```

The pinned vcpkg manifest feature `bluray` enables libbluray and its
dependencies only for this opt-in configuration. If using a different
package manager, provide `LIBBLURAY_INCLUDE_DIR` and
`LIBBLURAY_LIBRARY` at CMake configure time.

The release triplet builds **dynamic** libraries. The script stages the
correct libbluray and other dependent DLLs alongside the BDMV-specific app.
A normal `dotnet run` without `LapisNativeRuntimeDir` still uses the
standard file-only Native build.
Check vcpkg's `share/libbluray/copyright` and dependency licenses before
redistribution. Do not clone libbluray sources into Lapis.

The repository includes an **opt-in** GitHub Actions workflow
`libbluray opt-in build` (`.github/workflows/libbluray-opt-in.yml`).
While the workflow exists only in a feature branch, the GitHub Actions
`workflow_dispatch` button is not available (GitHub requires the workflow
file on the default branch). To validate PR #35 without merging it, **edit
the PR description to add `[verify-libbluray]`**. The PR `edited` event
will launch the optional backend build once; remove the marker afterward
to prevent unintentional reruns on later description edits. After the
workflow reaches the default branch, the Actions tab can dispatch it
directly against a selected ref. The normal PR pipeline never installs
the optional dependencies. This build checks compilation and link
compatibility, **not playback of a physical disc**.

## How the source integrates

```text
MediaLocator(Directory, "<disc root>")
    -> BluraySourceProvider
        -> libbluray: select main title
           -> IMediaByteStream (read/seek/size) -> FFmpeg custom AVIO
           -> INavigationController (metadata only)
    -> PlaybackMediaSession -> immutable GraphPlan -> PlaybackExecution
```

The C ABI v2 already models Directory locators, but only a build with
`LAPIS_ENABLE_LIBBLURAY=ON` registers this real source by default.
DesktopPlaybackHost routes Directory locators through the same PlaybackSession
actor and HWND binding as File locators; Avalonia never calls the disc API
directly. The library accepts an outer disc root containing
`BDMV/index.bdmv`, `BDMV/PLAYLIST`, and `BDMV/STREAM`, or the
`BDMV` subdirectory itself; both canonicalize to the same root.

Realistic disc backups may include `BDMV/BACKUP`, `BDMV/CLIPINF`,
`BDMV/BDJO`, `BDMV/JAR`, `BDMV/META/DL` artwork and
`bdmt_jpn.xml`, `CERTIFICATE`, and `MAKEMKV/AACS` or `MAKEMKV/CMAP`.
These are **not** additional titles in the library. Read-only Playlist
file indexing consults `BDMV/PLAYLIST`, not `BDMV/BACKUP/PLAYLIST`.
The presence of `MAKEMKV/AACS` alone does not establish the encryption
status of `BDMV/STREAM`.

The first implementation rejects discs reporting AACS or BD+. It only
exposes read-only Title/Playlist/Chapter/Angle metadata (where present).
Even if a disc advertises top menus or BD-J, Lapis does not claim to
support them. Direct `INavigationController::execute()` commands still
return NotReady while active, rather than mutating the native stream.

**Chapter seek control path (ADR 0039):** an optional read-only navigation
ABI v1 extension resolves the current Title's chapter ID into its canonical
start timestamp. The application actor executes `SeekChapterCommand` by
querying that timestamp at the *current* PlaybackEpoch and, only if it is
valid, advancing to a new Epoch through the normal seek lifecycle. This
does **not** implement a chapter-list UI, native BD-J menus or full
libbluray `bd_seek_chapter` transitions. Timestamp-based chapter navigation
on real clear BDMV playlists still requires fixture/hardware acceptance.

## Source override ordering

Native composition can register a custom `IMediaSourceProvider` before
opening any media. Explicit providers are evaluated first, most recently
registered first. When one claims a locator, a failure is returned directly;
the runtime does not silently retry through a different provider. This
also allows test fixtures to inject a fake Directory source while the real
libbluray provider is compiled in. Do not register providers while a source
or decoder generation is open.

## Verification boundaries

Default CI runs `bluray_source_contract` against a fake disc backend and
the real FFmpeg pipeline; this proves API boundaries and cleanup, **not**
that a physical Blu-ray disc or the optional libbluray binary works.
The libbluray ON build, valid clear-disc fixture, long seek loops,
multifile playlist transitions and platform/codec variations still
require hands-on acceptance before treating this as a usable feature.
