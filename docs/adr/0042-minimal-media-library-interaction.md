# ADR 0042: Calm library UI, dual views, contextual media management

- Status: Accepted for the next Avalonia desktop library vertical slice
- Date: 2026-10-19

## Product design and intent

The library should resemble a mature PC media catalog, not a dashboard or
an always-open video player. The Music Center for PC screenshots inform the
interaction density and restraint; they do not dictate music-specific
models, controls or branding.

- Library: **light**, neutral backgrounds, subtle dividers, low saturation,
  compact gold accent, stable 208-DIP left navigation and content-first header.
- Watch: remains **dark** with a native D3D11 child HWND. The library's light
  theme must not leak into Watch or put Avalonia overlays over that HWND.
- Disc cover: a **16:9** thumbnail from `BDMV/META/DL/*_L.jpg`, without
  pretending it is a portrait movie poster. Missing artwork uses a placeholder.
- A user explicitly selecting a library item is the only transition into
  the player. Browsing, search, sorting and view switching never open a media
  session.

## Functional design

`LibraryViewModel` owns the single filtered, sorted collection and stable
`LibraryCardViewModel` identities. Grid and List are two projections of the
same data, not two catalogs. Both support:

- Instant path/title search and All / Blu-ray / local video filters.
- Sort by recently imported (stable tie breaker), title ascending or title
  descending, with no invented duration, codec, HDR or year metadata.
- A single right-click action set: Watch, open the original in Explorer, copy
  the source path, inspect known catalog metadata, and **remove catalog
  entry only** after explicit confirmation. Deleting physical media from the
  UI is not supported.

Buttons displayed in the UI must have real handlers. Playlists and BD-J are
not advertised as interactive controls until the Native actor contracts
exist. No library actions bypass the playback actor.

## Accessibility, performance and limitations

- Grid artwork decoding remains bounded and asynchronous, shared between
  both presentations without repeated decoding on a layout switch.
- Grid/List view and filters preserve card identities and search text.
- Controls provide accessible names for view toggle actions. Keyboard
  navigation is inherited from Avalonia Button/MenuItem/ComboBox.
- This iteration renders collection items through ItemsControl; a large
  (thousands of entries) catalog will require a virtualized grid/list
  adapter, background scanning and a persistent presentation-preferences
  store. This is a known scaling task, not a reason to couple the library
  to Native MediaRuntime.
- Tests verify filtering, ordering, view-mode transitions and catalog-only
  removal. Real mouse/keyboard/menu appearance requires manual desktop QA.

## Separation

No changes to the C ABI, MediaTime, PlaybackEpoch, GraphPlan, HWND ownership,
D3D11 swap chain, libbluray source parsing, or the JSON catalog schema.
