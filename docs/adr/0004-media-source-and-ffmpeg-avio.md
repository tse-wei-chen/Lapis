# ADR 0004: Media sources own bytes; FFmpeg owns demuxing

- Status: Accepted
- Date: 2026-10-17

## Context

Letting FFmpeg open file paths and URLs directly would make the demux layer responsible for source transport. That is convenient for the first local file but creates a hard boundary problem when Blu-ray navigation needs to expose a virtual MPEG-TS stream, or when SMB, HTTP, protected content, caching, and diagnostics need source-specific behavior.

## Decision

Lapis separates source access from container parsing:

- `IMediaSourceProvider` selects a source implementation from an explicit `MediaLocatorKind`;
- a source session owns an `IMediaByteStream` plus source metadata such as a demux hint, with optional independently owned navigation under ADR 0036;
- local files are implemented by `FileSourceProvider` and `FileByteStream`;
- the demux hint is deliberately not the real path; it may preserve a suffix for format selection but cannot be used to reopen the source;
- FFmpeg receives bytes through a custom `AVIOContext` backed by `IMediaByteStream`;
- FFmpeg never opens the local path itself in the playback pipeline;
- FFmpeg packet/frame/context types remain inside the FFmpeg adapter folders, enforced by architecture tests;
- decoded video enters the core as `VideoFrame` with an abstract surface rather than as `AVFrame*`;
- the public runtime header is compiled as C17 in CI to keep the ABI genuinely C-compatible.

## Concurrency

Potentially blocking media operations such as source probing and seek execute outside the session state mutex. They capture an epoch, perform the work, then compare-and-commit only if that epoch is still current. A close or newer discontinuity can therefore invalidate late work without waiting on FFmpeg I/O.

## Consequences

Future Blu-ray, network, cache, or protection providers can feed the same FFmpeg demux adapter without changing demux/decode code. The extra AVIO adapter is intentional infrastructure, not a workaround. Expensive media work cannot hold the authoritative session-state lock, and late results are rejected at commit time.
