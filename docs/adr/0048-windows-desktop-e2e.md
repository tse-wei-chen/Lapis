# ADR 0048 — Black-box Windows desktop E2E and failure artifacts

- Status: Implemented; final hosted Windows runner validation required
- Date: 2026-10-20

## Goal and scope

Existing NativeAvaloniaPlaybackAcceptanceTests exercise real D3D11, HWND
attach/detach, seek, and a transparent overlay. They run **in the same
Avalonia process** and never press user-facing application buttons.
They are valuable, but not equivalent to a black-box UI end-to-end test.

`tests/Lapis.Desktop.E2E` launches the **actual built Lapis desktop EXE**
in an independent process. Windows UI Automation 3 through FlaUI controls
buttons by stable `AutomationProperties.AutomationId`, instead of brittle
Chinese/English text labels or fixed 4K pixel coordinates. This is a
real Win32 desktop/UIA test, not an Avalonia.Headless visual test.

Two initial journeys:

1. Start Big Picture -> invoke Manage -> ensure the Big Picture play
   entry is hidden -> send Esc -> ensure the Play entry returns.
2. Start Big Picture with a test video in an isolated catalog -> invoke
   Featured Play -> detect a separate owned `WatchChromeWindow` and its
   timeline and transport controls -> verify the real sidecar SRT cue ->
   activate Back -> ensure Big Picture is restored and the app stays alive.

Tests intentionally do **not** replace FFmpeg with a fake player, claim
simulated D3D11 audio/video correctness, or use a test-only direct UI
navigation shortcut. The catalog is populated through the real
`JsonMediaLibrary.ImportAsync` before the EXE starts.

## Isolation, test input and artifacts

The process-only `LAPIS_E2E_MODE=1` plus an absolute
`LAPIS_E2E_CATALOG` overrides the catalog location. This switch is
ignored in normal production use, protecting the user's AppData library.
Each test uses its own temporary directory, copies the committed Native
soak MP4 there, and creates an adjacent same-basename UTF-8 SRT.

The test runner needs:

- `LAPIS_E2E_EXE`: actual `Lapis.Desktop.Avalonia.exe` with all native
  dependencies staged beside it;
- `LAPIS_E2E_MEDIA`: an existing playable local MP4;
- `LAPIS_E2E_ARTIFACTS`: path for screen PNGs, UI Automation tree dumps,
  and xUnit TRX logs. These are uploaded even when the test fails.
Monitor rectangles come from Win32 `EnumDisplayMonitors`; Windows-only
`System.Drawing.Common` encodes PNGs. No Windows Forms dependency is enabled.

Run locally on Windows after the Native runtime has been built:

```powershell
$env:LAPIS_E2E_EXE = (Resolve-Path 'src/Lapis.Desktop.Avalonia/bin/Release/net10.0-windows/Lapis.Desktop.Avalonia.exe').Path
$env:LAPIS_E2E_MEDIA = (Resolve-Path 'native/MediaRuntime/tests/assets/test_soak_churn.mp4').Path
$env:LAPIS_E2E_ARTIFACTS = (Join-Path (Get-Location) 'build/e2e-artifacts')
dotnet test tests/Lapis.Desktop.E2E/Lapis.Desktop.E2E.csproj -c Release
```

The E2E project is invoked **explicitly** after the Native CI stage has
assembled the shipping application and dependencies. It is intentionally
not in the generic solution-wide managed test sweep, which may run before
Native binaries are available.

## Known verification boundaries

- Hosted Windows runners have a desktop capable of creating HWND/D3D11
  (verified by existing acceptance), but UIA accessibility and physical
  mouse/keyboard injection may still differ between runner images.
- Screenshots are useful diagnostics, not automatic perceptual judgments;
  an all-green E2E does **not** prove that a Composition animation is
  smooth, HDR colors are accurate, or Blu-ray PGS subtitles work.
- The committed Native soak video is short; this initial E2E does **not**
  claim that it verifies the three-second playing-only auto-hide timer.
  That merits a separate long-duration fixture and time-sensitive test.
- Real Explorer drag/drop with OS file transfer and a physical USB Xbox
  controller remain separate Windows hardware E2E milestones. The
  existing unit/integration tests cover path classification and gamepad
  semantic-input policy without claiming actual HID input was generated.
