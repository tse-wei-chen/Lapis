# ADR 0037: Optional libbluray backend for unprotected BDMV directory sources

- Status: Implemented (native opt-in preview)
- Date: 2026-10-19

## Context

ADR 0036 defined optional `INavigationController` metadata independently
of `IMediaByteStream`. The playback source registry only shipped FileSource.
The next milestone is to put real libbluray behind the existing Source
Provider boundary without coupling the FFmpeg demuxer, worker graph,
C ABI or GUI to `BLURAY*` and `BLURAY_TITLE_INFO`.

## Decision

1. Add `BluraySourceProvider` for `MediaLocatorKind::Directory`, supporting
   either a disc root containing `BDMV` or the `BDMV` directory itself.
   Require `BDMV/PLAYLIST`, `BDMV/STREAM`, and a recognizable index.
   Optical drives and ISO images are deliberately not accepted yet.
2. Isolate the actual libbluray C API in
   `source/bluray/libbluray_disc.cpp`. Its owner provides byte read/seek/size
   and an owned, read-only navigation snapshot (title indices, distinct MPLS
   IDs, chapters, angles, durations). libbluray's 90 kHz timestamps are
   converted to canonical nanosecond `MediaTime` with overflow protection.
3. The source session exposes one virtual `IMediaByteStream` for FFmpeg
   custom AVIO, with a fake .m2ts hint, and a separate navigation object.
   They share an adapter-owned disc lifetime. No libbluray pointer escapes.
   A per-disc mutex serializes byte read/seek/tell; FFmpeg remains the
   only demuxing owner.
4. Startup selects libbluray's main title (or a discovered longest-title
   fallback) **before** starting FFmpeg. The backend rejects encrypted
   AACS/BD+ discs, zero-title/no-stream data, and suspiciously large title
   or chapter counts. Do not enable menu/BD-J/overlay capability flags.
5. The initial main title must be present in successfully decoded title
   metadata. If the library's reported main title has no metadata, choose
   the longest available title; reject a completely empty list. This
   policy is a pure helper with default-CI regression coverage.
6. Navigation actions deliberately return `NotReady` instead of changing
   the live virtual MPEG-TS cursor behind a running FFmpeg demuxer. Actor
   coordination, epoch bumps, stream replacement, and menu events are
   deferred.
7. Explicitly registered source providers take precedence over the built-in
   providers, most recently registered first. Once a provider claims a
   locator, a failed Open is returned without fallback to another provider;
   transparent fallback could bypass a deliberate transport or protection
   policy. This resolves opt-in test backend injection, where both the real
   and fake Blu-ray providers claimed Directory locators.
8. Compile and register the real backend only with
   `LAPIS_ENABLE_LIBBLURAY=ON`. vcpkg manifest feature `bluray`
   supplies the external library; the normal/default dependency graph and
   CI remain FFmpeg-only. `BluraySourceProvider` itself always compiles,
   permitting fake disc backends in deterministic native regression tests.
9. The already existing C ABI v2 Directory locator can reach this provider
   **when** the opt-in backend is compiled. No new ABI exports, frontend
   directory picker or misleading BD-J controls are added.

## Verification

- A fake disc uses the normal PlaybackMediaSession -> custom FFmpeg AVIO path
  with the pre-existing Y4M media fixture. Tests verify directory detection,
  reading/seeking, owned title/playlist metadata, unsupported navigation
  operations, source replacement and final adapter destruction.
- The standard Windows Native CTest and Managed/WinForms integration suite
  must remain green with the default (libbluray OFF) configuration.
- An opt-in workflow `libbluray opt-in build` compiles and links the
  actual adapter with the pinned vcpkg feature and runs the source contract.
  Before the file is on the default branch, it can be triggered from this
  feature PR via a description edit containing `[verify-libbluray]`.
  After merge, it also supports `workflow_dispatch`. It is deliberately
  not part of regular PR CI and does not prove physical-disc playback.
- An opt-in Windows run compiled and linked libbluray 1.5.1, but its fake
  source integration test exposed built-in/explicit provider precedence.
  The override fix is covered in both default and libbluray-enabled tests.
- **Not yet verified by default CI:** the optional libbluray ON build and
  real unprotected BDMV playback. A manual workflow run is required for the
  former; the latter requires a legal test disc and playback acceptance.

## Next steps

Test the opt-in build with an unprotected disc fixture on Windows, add explicit
Blu-ray source selection to DesktopPlaybackHost without changing its renderer,
then implement actor-owned navigation commands and epoch/event boundaries.
Keep content-protection providers and BD-J/menu compositor independent.
