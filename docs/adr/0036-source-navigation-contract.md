# ADR 0036: Optional navigation controller in source sessions

- Status: Accepted (contracts only)
- Date: 2026-10-19

## Context

The `arch.md` source/navigation design separates virtual media bytes from
Blu-ray titles, playlists, chapters, angles, menu input and BD-J overlays.
The FileSource currently exposes only bytes to FFmpeg. Allowing libbluray
types or callback-owned graphics into the core would defeat adapter isolation.

## Decision

1. Define core-owned navigation capabilities, owned title/playlist/chapter/
   angle snapshots, typed commands and results, nonblocking navigation events,
   and separate premultiplied-BGRA menu overlay payloads.
2. Add an **optional** `INavigationController` to `MediaSourceSession`,
   independent from its mandatory `IMediaByteStream`. Destruction order
   releases the byte stream first so a virtual reader can borrow the
   navigation controller through its own destructor.
3. Let PlaybackMediaSession register source providers **before** opening a
   source and expose navigation metadata as a value snapshot; no raw
   navigation-controller pointer escapes the source session.
4. Native navigation revisions are distinct from PlaybackEpoch. A future
   application command must serialize navigation with demux/decoder
   teardown, advance PlaybackEpoch for stream discontinuities, and reject
   late events from the previous generation. Do NOT invoke execute() on
   the active stream through an independent UI thread.
5. A future libbluray adapter must copy callback buffers and queue events in
   bounded storage; callbacks (including JVM threads) must never call the
   renderer, FFmpeg or Application actor synchronously. A separate menu
   compositor, not SubtitleTimeline, owns menu overlays.
6. Keep protection and licensing separate. Navigation support does not
   imply AACS/BD+ playback. The public C ABI v2 and existing file-source
   behavior remain unchanged.

## Verification

A synthetic optical-disc provider feeds the existing small Y4M file through
custom AVIO and supplies fake navigation state. Tests cover source replacement,
owned snapshots, destruction order, late registration, capability checking,
command revision checks, nonblocking events and overlay memory bounds.
All existing FFmpeg/D3D11/WASAPI tests remain release gates.

## Deferred

Real libbluray and BD-J integration, optical disc access, protected media,
menu rendering, explicit actor commands and their epoch reconciliation,
event overflow policy, and a versioned managed/C ABI navigation extension.
