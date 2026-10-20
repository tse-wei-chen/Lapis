# ADR 0047 — Xbox-compatible controller navigation for Big Picture

- Status: Implemented, hardware validation pending
- Date: 2026-10-20
- Base: `develop` at `87eaae7e`; PR based solely on that branch

## Decision

Use Windows-inbox **XInput 1.4**, without an extra native dependency or
game controller NuGet package, for the first end-to-end controller slice.

The desktop UI owns a short-lived XInput polling loop while foregrounded.
It passes `GamepadReading?` samples through the deterministic
`GamepadInputRepeater`; views receive only semantic `GamepadAction` values.
Device indices, stick axes, packet formats and native DLL loading must not
reach the catalog, player actor or Blu-ray runtime.

- Up/down/left/right: D-pad or dominant left-stick axis. Analog dead zone
  is ~55%, and physical D-pad wins over analog drift.
- A: activate the focused Big Picture button/card; when nothing is focused,
  play the already-selected featured movie.
- B: reset to Home in Big Picture; from Manage return to Big Picture; during
  Watch, close the playback session and return to the same featured card.
- LB/RB: cycle All, Recently Added, Blu-ray, Videos, preserving identities.
- Menu/Start: open Manage. In Watch, toggle the existing read-only Blu-ray
  playlist sidebar (if a disc has playlist names).
- Watch: A pauses/resumes, D-pad left/right uses the **existing actor seek**
  commands, B returns to Library. No new direct Native calls.
- Directional hold repeats after 330ms, at most once per 110ms after that;
  A/B/Menu/shoulder buttons are edge-triggered and never auto-repeat.
- XInput controller slots 0–3 are supported. On disconnect the service emits
  a missing reading before adopting a new controller to reset stuck buttons.
- Polling pauses whenever the main app / owned overlay is not active.
  Dialogs and video may not be activated by background gamepad input.
- Direction-key navigation now includes all large, actionable top navigation,
  import, Manage, hero and poster buttons, not just the poster grid. Keyboard
  uses the same geometry-based navigation but respects text box/ComboBox input.
- All of these operations remain above the playback/native C ABI boundary.

## Controls and composition

Large navigation focus uses Avalonia Focus styles and the existing render
thread library composition entrance motion; it never animates a
NativeControlHost or D3D11 HWND. The original selected artwork is retained
when moving from Watch back to Big Picture. The initial focused movie is
restored after the collection has been laid out, not before the async catalog
load completes.

## Testing and constraints

Automated pure tests cover left-stick dead zones, D-pad priority, action
edges, hold-repeat cadence, direction changes and device disconnection;
existing focus geometry and movie identity tests remain in place.
Windows CI builds the real WPF/Avalonia application and also checks native
D3D11 playback when triggered; **XInput hardware interaction itself is not
validated by a synthetic CI input**.

XInput 1.4 works with Xbox/XInput-compatible controllers. DirectInput-only
devices and a native DualSense implementation are **not** claimed.
Steam Input may be able to emulate an XInput device, but duplicated keyboard
mappings and other per-user middleware configurations need manual validation.

Next hardware acceptance: Xbox pad through unplug/replug, bumper navigation,
mouse-to-gamepad transition, 1080p/4K DPI, popup focus, Watch/Library return,
controller hot-plug, reduced-motion settings and long-press cadence.
