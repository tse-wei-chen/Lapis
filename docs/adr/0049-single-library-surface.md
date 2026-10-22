# ADR 0049 — One Library Surface with Two Presentations

Status: Accepted

## Decision

The desktop shell mounts only `BigPictureLibraryView` for browsing. It may
project `LibraryViewModel.VisibleItems` as either artwork cards or a dense list.
A presentation switch never navigates to a second management page and never
instantiates another catalog/view model.

- Filtering, sort, selection, bookmarks, favorites, importing and all mutations
  use the existing `LibraryViewModel` and `MainWindow` application services.
- Movie information, renaming, source repair and removal are provided by the
  existing Big Picture detail sheet; context menu entry opens that sheet.
- Gamepad Menu swaps grid/list mode; Back still closes details or returns
  to the All category, and D-pad navigation selects visible cards in either mode.
- The top toolbar is persistent navigation, not a second page.
- The full-screen Watch/native video surface remains a separate physical screen
  due to Win32 D3D11 airspace and NativeControlHost constraints.
- Both visual projections are bound to the same `VisibleItems` instance.
  Switching display mode preserves category, search, sort, and selected movie.
- The theme uses FluentAvaloniaUI 2.5.1 with Avalonia 11.3.22
  and an application-specific dark cinema palette. Avalonia 12 is not required.

## Verification

Run `dotnet test` for managed tests on a Windows .NET 10 agent and run
the desktop E2E test `BigPicture_Grid_And_List_ShareOneLibraryPage`.
Check grid/list switching, screen-reader navigation, keyboard/gamepad focus,
details actions, import/drop, playback/full-screen return and 125%–200% DPI.
