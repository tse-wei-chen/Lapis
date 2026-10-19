# ADR 0043 — Quiet library palette and compositor-only motion

- Status: Implemented, manual visual QA pending
- Date: 2026-10-19

## Reference and concerns

The provided 14-second Music Center for PC screen recording demonstrates
its catalog switching between grids and rows with no large hero, flashing
transition or shifting toolbar. This is a **reference recording**, not a
recording of Lapis itself. It cannot establish specific Lapis bugs.

The prior library UI had an inconsistent mixture of gold accents and hardcoded
warm/gray colors, duplicate import navigation, and layout toggles without
deliberate micro-interaction. Its D3D11-backed Watch page must not be animated
by Avalonia Composition.

## Decision

- Library palette is **cool graphite / muted petrol** instead of gold:
  accent `#287584`, stronger `#1F5E6D`, selection `#E2EFF1`,
  canvas `#F5F7F8`, surface `#FFFFFF`. Colors are defined once as
  named brushes in the Window resources for later personalization.
- Keep Watch dark and all playback primitives entirely unchanged.
- View enter uses Avalonia `ElementComposition.GetElementVisual` with
  compositor `Opacity` and small `Offset` animations (155ms). It starts
  after the layout is active; no control is copied/recreated by the
  animation, no task delay or animation-driven actor call.
- Card/list hover uses a render-thread **implicit** opacity animation
  (115ms) using the visual's current and final values. Separate XAML
  pseudo-state styles own persistent selected/focus colors, not fake animation.
- Only **library** viewports and library entries are eligible. Never
  animate a `NativeControlHost` or a child HWND.
- Motion obeys Windows `SPI_GETCLIENTAREAANIMATION` and the
  `LAPIS_REDUCE_MOTION=1` override for testing/accessibility. When disabled,
  updates are immediate. `LibraryMotion.ShouldAnimate` is unit tested.

## Follow-up

- Manual UI QA on Windows at 100%, 125%, 150% DPI and reduced motion;
  measure Grid/List scroll retention and rapid repeated view switches.
- Virtualize grids for thousands of titles (separate from visual motion).
- A proper user-selectable theme palette (rather than a single fixed default)
  is future work. No claims about native Blu-ray playback are made here.
