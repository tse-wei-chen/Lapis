# ADR 0045 — Immersive Watch chrome across the Win32 airspace boundary

- Status: Draft implementation; real multi-monitor hardware UX acceptance required
- Date: 2026-10-20

## Problem

The original Avalonia Watch page permanently reserved space for a top
title bar, lower transport/status rails and a right-hand MPLS index. It did
not enter immersive fullscreen and could not place true transparent subtitles
over the D3D11 image because `WindowsVideoSurface` is a Win32
`NativeControlHost` and its native child HWND occupies a distinct airspace.

## Decision

- Enter Watch in `WindowState.FullScreen`, keeping the SAME persistent
  `WindowsVideoSurface` and native D3D11 child HWND; video itself has no
  margins, framing border or reserved transport/status rows.
- Use an **owned transparent Avalonia top-level window** for the overlay
  chrome. The Win32-owned video HWND is **not** parented into the overlay.
  This allows translucent Watch header/bottom bar and in-place Disc panel
  without assuming XAML can draw over a NativeControlHost.
- Auto-hide the header and timeline about 3s after pointer inactivity while
  Playing. Reveal on pointer motion or when paused. Pointer sampling uses
  GetCursorPos so motion is detected even when the native HWND handles the
  pointer messages. Preserve controls during visible disc navigation.
- Keep a separate transparent subtitle text layer at the video bottom when
  chrome is hidden. The first end-to-end timed source is a **local SRT sidecar
  with the same basename as a File-source movie**; malformed/oversized SRT
  is safely ignored and no fake Blu-ray PGS rendering is claimed.
- Retain the Playback Actor as the sole transport/seek owner and the
  existing BeginScrub/CommitScrub epoch-safe seek contract.
- Esc exits to Library, F11 toggles fullscreen/maximized, Space/K and
  J/L or arrows delegate to existing player commands.
- The Disc sidebar lists actual MPLS filenames only, explicitly marked
  read-only pending Native Title/Playlist/Chapter API.

## UX and platform verification still required

CI can verify managed compilation, time-based SRT unit tests, CTest and
the native D3D11 HWND lifecycle. It cannot prove that a transparent **owned
Win32 popup** composites reliably above the D3D11 surface on every
Windows composition backend, HDR monitor, DPI scaling/multiple displays, or
remote desktop. In particular, validate chrome focus/keyboard routing,
overlay sizing across fullscreen transitions, subtitle contrast and cursor
interaction. If native composition airspace remains inconsistent, move to
a dedicated native video/overlay compositor with GPU-integrated subtitles.
Do not attempt ordinary semi-transparent XAML children above the HWND.
