# ADR 0033: First desktop shell is an adapter above a UI-neutral playback host

- Status: Accepted
- Date: 2026-10-18

## Context

ADRs 0030–0032 established a render-thread-affine HWND surface-control
contract, C ABI v2 binding, and a real D3D11/DXGI presenter. The repository
still had no runnable desktop application to create a HWND, invoke the actor,
and own native teardown in the right order.

Binding presentation directly from WinForms event handlers would couple GUI
callbacks to synchronous P/Invoke commands and destroy/recreation races.
Mixing WinForms types into Application would also preclude future UI hosts.

## Decision

1. Add `Lapis.Desktop.Hosting` as a portable .NET library, referencing only
   Application and Contracts. `DesktopPlaybackHost` composes the existing
   `PlaybackSession` with an independent `IVideoOutputSurfaceBinding` port.
2. Serialize Open/Bind/Play, Pause/Resume, Seek, Stop/Detach, and window
   replacement through one host command gate. Surface binding obtains the
   actor's current playback epoch and does not advance it.
3. Dispatch surface changes through Task.Run: ABI v2 binding is synchronous
   and can block waiting for render-thread acknowledgement, so never perform
   it directly on a UI dispatcher.
4. Host disposal first gates new commands, finishes in-flight commands, then
   detaches the HWND and disposes the playback actor. If detach fails,
   actor/native disposal still joins the render thread before window teardown.
5. Add `Lapis.Desktop.WinForms` as the first concrete Windows presentation
   adapter (net10.0-windows); it owns the Panel HWND and file dialogs, but
   does not own decoder, D3D, C ABI/PInvoke, or domain state transitions.
6. The FormClosing event defers HWND destruction until host disposal completes.
   A native teardown failure keeps the form alive rather than destroying a
   possibly borrowed HWND.
7. Add a small local-file UI with Open, Play/Pause, Stop and seek-to-seconds,
   plus drag-and-drop and basic keyboard shortcuts.
8. Copy the native build's Release DLL beside the desktop executable when
   present. Do not make the managed CI depend on a local native build.
9. Add portable fake-port tests for Open/Bind/Play sequencing, epochs, Stop,
   replacement, invalid input, and detach-failure disposal. Add architecture
   tests ensuring the hosting library never depends on WinForms/native code.

## Deferred

- Native execution snapshots/events reconciled into Application states, current
  position/duration, native Ended/Faulted recovery and playback progress slider.
- Robust HWND recreation/DPI transitions and window replacement UI.
- Installer/publish packaging and DLL dependency discovery.
- BDMV title/playlist navigation, Blu-ray menus, subtitle UI and AI overlays.
- GPU decode, HDR/color management, audio device controls and quality tuning.
