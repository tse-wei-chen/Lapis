# ADR 0038: Desktop opens typed media locators through one playback actor

- Status: Implemented for the media locator; desktop adapter superseded by ADR 0040
- Date: 2026-10-19

## Context

The native source contract and C ABI v2 already model File and Directory
media locators, while `DesktopPlaybackHost.OpenAndPlayAsync(string)` and
`PlayerForm` only supported file paths. Sending BDMV input from the UI by
calling libbluray or a second Native open path would violate the single
PlaybackSession actor, playback epoch and HWND lease ownership contracts.

## Decision

1. Preserve the existing file-path host overload for compatibility. Add
   `OpenAndPlayAsync(MediaLocator, HWND)` as the common path for all supported
   source kinds. The host still serializes Open -> Bind(HWND at current
   PlaybackEpoch) -> Play and never probes or decodes the source itself.
2. Add a distinct BDMV directory picker to the WinForms header and empty
   screen plus Ctrl+Shift+O. Drag-and-drop accepts files and local directories.
   UI-only directory validation accepts disc roots and BDMV subdirectories
   with index.bdmv (or index.bdm), PLAYLIST and STREAM. The native adapter
   remains authoritative and can still reject protected/invalid media.
3. Store the full `MediaLocator` when an Open succeeds. A replay after EOF
   reopens the same kind, not an accidentally downgraded File locator, and
   creates a fresh playback generation through the actor as before.
4. Presentation-only labels distinguish BDMV preview from ordinary files.
   BDMV entry points are visible in the default build, but do **not**
   advertise ready-to-use protected disc, menu or title switching support.
   Opening a directory requires Native MediaRuntime built explicitly with
   `LAPIS_ENABLE_LIBBLURAY=ON` and deployed with its dynamic dependencies.
5. Native Source Registry, FFmpeg, D3D11, WASAPI and C ABI v2 remain
   unchanged. Navigation commands remain gated behind future actor-owned
   discontinuity/epoch handling.

## Regression coverage

- Host tests verify the typed Directory locator reaches the runtime intact
  and retains the same epoch-ordered Open/Bind/Play sequence after Seek/reopen.
  Existing file-path and shutdown tests stay unchanged.
- Pure WinForms tests validate BDMV directory selection of roots/children,
  rejecting unsupported paths; STA Chrome tests exercise new accessible
  buttons without requiring libbluray.
- Default Windows CI verifies compatibility, not real-disc playback.
  The separate successful libbluray opt-in build/test verifies Native linkage
  and a fake-provider-backed FFmpeg custom-AVIO test, not a real Blu-ray.

## Historical note

The WinForms BDMV picker and tests were removed; the Avalonia
`BdmvLocator` service and tests now own UI-side directory validation.
The native source and session actor boundaries remain unchanged.

## Deferred

A native capability handshake for actual Directory readiness, real clear-disc
hardware fixture tests and BDMV path variants, source-protection feedback,
Title/Chapter switch commands with PlaybackEpoch transitions, menus/BD-J
rendering, and installer packaging/licensing of libbluray dependencies.
