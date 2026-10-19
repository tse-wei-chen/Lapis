# Preview: unprotected Blu-ray BDMV directory source

Lapis's native media runtime can optionally open an **unprotected** Blu-ray
directory as a virtual MPEG-TS stream through libbluray. This is NOT full
PowerDVD-equivalent disc playback: there is no BD-J, menu rendering, actor-
coordinated title/chapter switching, optical drive access, ISO support, or
AACS/BD+ decryption. The WinForms Open dialog is still file-only.

## Enable explicitly (Windows, x64)

From a PowerShell terminal at the repository root with a vcpkg checkout:

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

The release triplet builds **dynamic** libraries. Make the required
libbluray and dependent runtime DLLs available to the final executable.
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
Directory input is not yet wired to the desktop file-selection UI.
Point to a disc root with `BDMV/index.bdmv`, `BDMV/PLAYLIST`
and `BDMV/STREAM`, or to its `BDMV` subdirectory.

The first implementation rejects discs reporting AACS or BD+. It only
exposes read-only Title/Playlist/Chapter/Angle metadata (where present).
Even if a disc advertises top menus or BD-J, Lapis does not claim to
support them. Any navigation command returns NotReady while active,
rather than mutating the native stream outside PlaybackEpoch control.

## Verification boundaries

Default CI runs `bluray_source_contract` against a fake disc backend and
the real FFmpeg pipeline; this proves API boundaries and cleanup, **not**
that a physical Blu-ray disc or the optional libbluray binary works.
The libbluray ON build, valid clear-disc fixture, long seek loops,
multifile playlist transitions and platform/codec variations still
require hands-on acceptance before treating this as a usable feature.
