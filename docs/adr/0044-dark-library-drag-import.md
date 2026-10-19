# ADR 0044 — Dark cinema library and typed Explorer drop import

- Status: Implemented, desktop interaction QA pending
- Date: 2026-10-19

## Decision

1. Prefer a content-first **dark library** to the previous white theme, with
   centralized charcoal/navy/petrol palette tokens and Avalonia Dark
   ThemeVariant for native platform controls. The Watch view is already dark;
   its NativeControlHost, ABI and playback life cycle remain unchanged.
2. Allow drag/drop of Explorer files and folders **on the Library only**.
   Display a temporary, input-transparent visual overlay while file data is
   dragged across the Library. OS drop cursor reports Copy, but the app
   **indexes metadata only and never copies video bytes**.
3. A pure LibraryDropClassifier checks local source existence and a
   conservative supported-extension allowlist. Directory items must validate
   as complete BDMV roots, or their BDMV child, using the **existing**
   LibrarySourcePaths.Normalize contract. Never recursively descend arbitrary
   dropped directories, infer AACS/BD+ status, or ingest BACKUP/MAKEMKV as films.
4. Canonicalize before de-duplicating dropped locators; then IMediaLibrary
   performs persistent cross-session de-duplication. The library batch API
   updates the card collection only once, reports additions, existing entries
   and skipped items, and tolerates individual source failures.
5. Limit one drop to 200 entries. Unknown/unavailable/URI-only dropped items
   are rejected explicitly, instead of silently becoming fake movies.

## Testing and caveats

Tests cover mixed files/discs, root-vs-BDMV-child duplicates, invalid folders,
image/MPLS/ISO rejection, batch limits, persistence and original file safety.
Native player tests must still pass unchanged. Real Windows Explorer drag/drop
behavior at different DPI and file-system permissions needs manual QA; no
protected Blu-ray/BD-J claims are made.
