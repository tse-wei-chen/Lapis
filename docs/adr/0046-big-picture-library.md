# ADR 0046 — Big Picture-inspired cinema-first library

- Status: V1 implemented; native controller integration and 10-foot hardware QA pending
- Date: 2026-10-20
- Reference: Valve's Steam Big Picture / Steam Deck library design, including
  its October 2026 Big Art Mode. Inspiration is interaction structure, not UI
  assets, logos, typefaces, marketing copy, or source code.

## Product decision

The primary collection view is no longer a dense file-management dashboard.
On application start, Lapis opens a **maximized, dark, content-led library**:

- Oversized 16:9 selected-title hero, backed exclusively by artwork imported
  from the user's own BDMV folders; simple quiet placeholder if none exists.
- Horizontal **Recently Added** shelf and a 16:9 All Titles wall.
- Large, discoverable navigational categories, an explicit focus ring, actual
  keyboard directional geometry navigation, and visible bottom hints.
- Enter / click activates the same playback operation as existing Library.
  Browsing or focusing never instantiates Native MediaRuntime.
- Existing management table, context actions and import pickers are retained as
  a deliberately secondary **Manage** screen. ESC returns from it; F11 toggles
  maximized/fullscreen. Video Watch is separately owned by PR #36.
- A folder import is explicitly marked Blu-ray rather than silently treating
  any folder as a movie; Explorer drag/drop and BDMV canonicalization use the
  existing catalog.

## Architecture

`BigPictureLibraryView` is an Avalonia presentation UserControl: UI focus,
navigation and selection only. It emits Play, Import and Manage requests to
MainWindow, which continues to own picker, catalog and playback actor boundaries.
`LibraryViewModel` owns stable cached LibraryCardViewModel instances,
filtered recent shelf and a highlighted featured card. Both shelf and wall
share card identities; category/search updates never create new catalog records.

`BigPictureFocusNavigator` is a pure geometry-based directional selection
algorithm, testable without Windows, D3D11 or the Avalonia visual tree.
The view delegates physical key navigation to this utility. Future XInput /
SDL GameController adapters should emit the exact same directional intents.

Existing composited entry motion is used for hero changes; BrushTransitions
handle navigation and tile hover/focus state. Reduced-motion remains respected.
No animation moves the NativeControlHost or GPU HWND.

## Tradeoffs and required next iterations

- **No gamepad adapter yet**. Keyboard arrow / Enter is the foundation, not
  a claim of complete Xbox/DualSense support. Controller-specific focus,
  repeat/deadzone, B/A/Menu mappings and controller hotplug need real testing.
- Large catalogs still use ItemsControl + WrapPanel, not a virtualized layout.
  Adding virtualization and background poster prefetch is necessary before
  positioning this as thousands-of-items-ready.
- Selected title backdrop uses the already decoded thumbnail for a subdued
  background rather than a separate HD fan-art source. Later add an opt-in
  high-resolution hero-art source, with bounded cache / decoding and no
  invented metadata or external art copying.
- Manual Windows QA required for 1080p/4K, DPI 100/125/150%, mouse/keyboard
  focus, scroll preservation, low-end GPU, HDR, and shell-to-Watch transition.

## Non-goals

No C ABI, libbluray parser, FFmpeg, D3D11 presenter, translation overlay or
subtitle behavior change. No retroactive modifications to the physical video
library or catalog schema.
