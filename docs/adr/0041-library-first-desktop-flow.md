# ADR 0041: Library-first desktop shell and disc-as-one-item import

- Status: Accepted for the first product-facing library vertical slice
- Date: 2026-10-19

## Context

The early Avalonia shell opened directly into the NativeControlHost playback
surface. A cinema application needs a personal media library as its home,
while the full-screen/theater watching experience is a separate mode.
Importing a BDMV directory as a single .m2ts file destroys Title/Playlist
and navigation context.

## Decision

1. Start on an independent **Library** page. Do not construct a native runtime
   or open a playback session until a user selects a stored library entry.
2. Import a local video as one catalog item; import a clear BDMV **disc root**
   as one catalog item, accepting either the outer root or its BDMV child as
   equivalent paths. Preserve the directory structure and normalize both
   selections to one disc root. Never copy the movie at import.
3. Add `Lapis.Library` with a third-party-neutral `IMediaLibrary` port and
   persistent atomic-replace `JsonMediaLibrary` v1 adapter in LocalAppData.
   The catalog uses versioned serialization; malformed data must fail
   explicitly instead of replacing/deleting the user's library.
   Large-scale indexed storage (SQLite), metadata agents and artwork can be
   introduced behind this port without changing playback contracts.
4. Isolate `LibraryViewModel` from NativeControlHost, playback snapshots,
   FFmpeg and libbluray. Search/filter the local catalog; only explicit
   **Watch** hands a typed MediaLocator to the existing PlayerViewModel actor.
5. Keep the player page collapsed while browsing the library. Upon entering
   Watch, attach the Avalonia NativeControlHost first, then bind its persistent
   HWND to the same DesktopPlaybackHost; leave by closing the actor session.
6. For discs, display only filesystem-derived MPLS filename identifiers as a
   **read-only preview**. This is not parsing, title enumeration, playlist
   selection, chapter navigation or Blu-ray menu rendering. Future commands
   must travel through the source-owned Navigation contract and actor.
7. Real Blu-ray playback still requires an opt-in libbluray-enabled native DLL;
   commercial AACS/BD+ titles, BD-J, ISO, optical-drive access, poster
   scraping and AI translation are not falsely advertised.

## Deferred contracts / release gates

- Disc/title/playlist metadata synchronization, chapter selection, resume by
  content fingerprint and path relocation semantics.
- Artwork cache, metadata scraper, virtualized library tiles and SQLite
  catalog adapter for very large collections.
- A composited video/subtitle/BD-J GPU presenter; Avalonia must not position
  transparent text over a child HWND.
- Real optical disc, device-loss and multi-monitor hardware acceptance.

## Testing

- Round-trip persistence, one entry per BDMV root, no-content-copy imports,
  invalid source validation, accidental overwrite prevention and delete from
  catalog without deleting original source.
- LibraryViewModel search/filter/empty state, plus existing native Avalonia
  acceptance explicitly activates Watch before testing the HWND lifecycle.
