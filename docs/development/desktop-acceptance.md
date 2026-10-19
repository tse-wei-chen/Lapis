# Desktop real-Windows acceptance (PR 35)

This matrix is a release **gate**, not evidence that hardware tests have already
passed. Managed CI cannot reproduce monitor power transitions, PMv2 HWND churn,
GPU reset, display driver replacement, WASAPI endpoint loss, or multi-hour
D3D swapchain behavior. Record OS build, GPU model/driver, monitor scaling,
audio endpoint, media sample fingerprint, log and pass/fail for every run.

## Prerequisites

Build native Release + managed Release following [desktop-host.md](desktop-host.md).
Use a locally licensed/clear local video with both audio and video, plus a short
2–10-second clip for repeat/EOF tests. Keep Task Manager GPU/memory/handles
visible and capture an app crash dump on a crash. Use a second display at a
different DPI scale when available. Do not use protected Blu-ray media to
evaluate this milestone.

## Required acceptance scenarios

| Case | Actions | Pass condition |
| --- | --- | --- |
| A01 Continuous playback | Play a 2-hour or longer file to EOF; repeat overnight if feasible | Video/audio continue without crash, app reaches Ended; no sustained resource growth |
| A02 Seeking | Seek backwards/forwards 50 times, including near EOF, paused and playing | No old frames/audio after seek; new epoch reaches requested time |
| A03 Stop/reopen | Stop, open same file, open a different file, repeat 30 times | Old swapchains and workers release; latest file plays; UI position resets |
| A04 Natural EOF | Let a short clip reach Ended, press 重播 and Space in separate runs | Playback restarts at 00:00 with a new epoch, no stale EOF |
| A05 Avalonia HWND reattachment | Exercise repeated NativeControlHost detach/reattach while playing | The *video* HWND stays identical as the proxy is recycled; no deadlock/black screen |
| A06 Resize/minimize | Resize aggressively, minimize/restore and maximize repeatedly | No stretch corruption, DXGI failure or crash |
| A07 Per-monitor DPI (not yet guaranteed) | Move Avalonia window between 100% and 150/200% monitors, change scale while playing | Video HWND remains valid and rendering continues after automatic NativeControlHost proxy reattachment. **Hardware gate remains open.** |
| A08 GPU Device Lost | Trigger adapter reset/device removal in a controlled development environment; capture native debug output | Transient loss recovers within bounded attempts **with a subsequent successful frame** or ends with a visible terminal failure; record HRESULT, removed reason and recovery attempts; no infinite loop |
| A09 Display disconnect | Disconnect/reconnect external display and switch graphics mode | No use-after-free or permanent deadlock; recover/fail clearly |
| A10 Audio endpoint churn | Unplug/replug output, change default device during playback | No process crash; fault is observable if recovery unavailable |
| A11 App shutdown | Close while opening, seeking, paused, full screen, and during NativeControlHost reattachment | Native render worker joined before HWND is destroyed |
| A12 Rapid switching | Alternate 10 different local files rapidly with mixed codecs | No previous content on newly opened title, bounded handle/VRAM usage |

## Automated native Windows soak runner

The Release native build contains `Lapis.PlaybackSoak.exe`. Unlike the
Avalonia desktop UI, this is a **console-driven repeatability harness** that
calls the existing **C ABI v2** through a real, visible Win32 output HWND.
It executes FFmpeg decode, the real native workers, D3D11 swap-chain binding,
natural EOF, and early Stop/Close without synthetic presenter mocks.

Run from the repository root in Windows PowerShell, after building native Release:

```powershell
# Repeat 100 Open/Play/Close cycles; alternate natural EOF and early Stop.
.\build\native\Release\Lapis.PlaybackSoak.exe `
  --media "C:\Samples\short-clear-video.mp4" `
  --cycles 100 --mode mixed --timeout-seconds 30 `
  --json ".\lapis-soak-100.json"

# Continuous actual clip-to-EOF playback (A01 candidate).
.\build\native\Release\Lapis.PlaybackSoak.exe `
  --media "C:\Samples\long-clear-video.mkv" `
  --cycles 1 --mode eof --timeout-seconds 10800 `
  --json ".\lapis-soak-long.json"

# A02/A03/A12 native epoch + HWND churn with two files (use clips >1 s).
.\build\native\Release\Lapis.PlaybackSoak.exe `
  --media "C:\Samples\movie-one.mkv" `
  --media-alternate "C:\Samples\movie-two.mp4" `
  --cycles 100 --mode churn --seek-ms 250 `
  --timeout-seconds 30 --json ".\lapis-soak-churn.json"

# A05 native simulation: keep one D3D HWND while recreating its parent.
# The video itself runs to EOF after 40 parent-window recreations.
.\build\native\Release\Lapis.PlaybackSoak.exe `
  --media "C:\Samples\short-clear-video.mp4" `
  --cycles 1 --mode parent-churn --parent-recreates 40 `
  --timeout-seconds 30 --json ".\lapis-parent-churn.json"

# 2000 rapid Open/Play/Close sequences without waiting for EOF.
.\build\native\Release\Lapis.PlaybackSoak.exe `
  --media "C:\Samples\short-clear-video.mp4" `
  --cycles 2000 --mode stop --timeout-seconds 30 `
  --json ".\lapis-soak-stop.json"
```

Parameters: `--media` is required and accepts Unicode paths; `--cycles`
is 1–10,000; `--mode` is `mixed`, `eof`, `stop`, `churn`,
or `parent-churn` (default mixed); `--media-alternate` is an optional second source used on
odd-numbered cycles; `--seek-ms` is a strictly positive forward seek target
in milliseconds (1–60,000, default 250) in `churn`.
`--parent-recreates` specifies 1–1000 parent HWND replacement attempts
per playback cycle (default 16) in `parent-churn`.
`--timeout-seconds` is a per-cycle EOF **or GPU-initialization** deadline
(1–86,400 seconds), not an elapsed-time quota; `--json` writes a report to a
user-specified path.
The report is also printed to standard output. **No media filename or
media path appears in the report**. For long runs, use a known,
locally permitted video; the automated CI fixture is video-only.

The JSON summary includes attempted/completed cycles, reason for failure,
elapsed time, baseline/final/peak process **Private Bytes**, working set,
Windows handle count, GDI objects and USER objects. It retains at most
128 resource checkpoints, plus the session's recent 128 GPU diagnostic
events and overwrite count. `successfulSeeks` and
`appliedOutputRebinds` quantify the extra live-control coverage in
`churn` mode; `completedParentRecreations` counts safely completed
parent HWND destruction/re-attachment transitions. In `parent-churn`,
the D3D child HWND is unchanged, temporarily parked in an independently
owned hidden popup, then attached to a newly created container HWND.
The old container is destroyed **only after** parking. No Native surface
rebind occurs, and playback must continue to natural EOF after all
transitions without reporting Faulted. This is a native simulation of
the proxy-parent HWND lifetime design, **not a real DPI/Avalonia integration test**.
A `churn` cycle first waits for a *new* real D3D11
swap-chain creation event, creates a distinct visible output HWND, requests
a synchronous renderer-acknowledged rebind, destroys the old HWND **only
after APPLIED**, then seeks forward to the requested timestamp and back to
zero using two new playback epochs. The UI thread keeps pumping Win32
messages while native control commands execute off-thread. No fixed leak threshold is baked in: driver
caching, WARP, Windows session type and codec are variable. Review
multiple repetitions and sustained trend rather than assuming every
increase is a memory leak.

CI invokes `windows_native_playback_soak_quick` with **4 fixture cycles**
(two natural EOF, two early close) and
`windows_native_playback_churn_quick` with **2 cycles** of real output HWND
replacement, forward/backward seek and alternating two distinct synthetic
indexed MPEG-4 MP4 files (30 frames at 5 fps each). A separate
`windows_native_playback_y4m_seek_quick` test exercises the small,
prefetched raw Y4M Seek regression through the real C ABI and D3D11 path.
`windows_native_playback_parent_churn_quick` runs a six-second video to
natural EOF after 16 parent HWND recreations with the same persistent
rendering HWND. A green result proves only these short Windows CI runner
scenarios; it **does not**
exercise actual PMv2 multi-monitor DPI, protected Blu-ray, large HDR frames,
real driver removal, multi-hour A01, or audio-device churn. The runner uses
a plain Win32 output HWND and does not replace A05/A07 Avalonia NativeControlHost and hardware DPI tests.

## Real D3D11 resource recovery with test-only DeviceLost injection

The Windows Native CTest `d3d11_live_device_loss_recovery_injection`
creates a **real D3D11 device and SwapChain** on a Win32 HWND, decodes
real media through `PlaybackExecution`, then uses a test-only
`IVideoPresenter` decorator to return `DeviceLost` after a real frame
was successfully presented. The underlying production D3D11 Presenter
still performs every actual resource release, device creation, SwapChain
creation, and post-recovery Present.

It verifies three independent paths:

- **Transient fault:** one injected DeviceLost, one successful rebind, a
  subsequent successful real Present, and a clean natural EOF.
- **Persistent faults:** after two rebuild attempts the third DeviceLost
  must terminate in `PresenterUnavailable` with a diagnostic terminal event.
- **Fault on last frame:** successful resource rebind with no following
  Present must terminate as unavailable, **not falsely report Ended**.

The test also checks device/SwapChain creation counters, recovery journal
events, and Close releasing resources before the test destroys the HWND.
This is a **synthetic DeviceLost result at the native Presenter boundary**,
not an actual OS GPU reset. Physical GPU device removal and driver recovery
remain A08 release blockers and cannot be certified by green CI.

## WASAPI endpoint-loss event detection and controlled reopen

WASAPI uses the audio-event notification stream to avoid per-buffer
polling during normal playback. When a device is unplugged or the audio
service invalidates its endpoint, the driver may **stop signalling
the event**; an `INFINITE` Win32 wait could therefore leave the audio
worker blocked until a user-initiated Stop/Close.

`WasapiSharedAudioOutput::wait_until_writable` now uses a stop-aware
100 ms event wait interval. Both a delivered event and a timeout verify
`IAudioClient::GetCurrentPadding`, mapping invalidated endpoints to
`DeviceLost`; it only returns `Ready` when buffer space exists.
The 100 ms probe is a **maximum scheduled polling interval**, not a
guaranteed wall-clock recovery deadline if a driver COM call itself
hangs. A stop_token still signals a separate Win32 event immediately.

The new Windows `wasapi_endpoint_loss_event_wait` CTest executes the
same internal event-wait routine with simulated callbacks, covering:
loss with no event, readiness without an event, spurious event while
full, generic device error, and stop interruption. It does not
disconnect or manipulate Windows' actual sound devices.

The `playback_execution_smoke` test additionally forces DeviceLost
during audio Submit and asserts observable `OutputUnavailable`,
reset/close of the old output, and normal decode/drain after a
**caller-driven close and reopen on a new epoch**. This is not seamless
automatic audio endpoint migration. A10 remains an explicit physical
headphone-unplug/default-device-switch acceptance gate.

## Real Avalonia + Native end-to-end acceptance in Windows CI

The Native CI job builds `Lapis.Desktop.Avalonia.Tests` and runs the
opt-in `NativeAvaloniaPlaybackAcceptanceTests` with a real native DLL
when `LAPIS_AVALONIA_NATIVE_ACCEPTANCE=1` and `LAPIS_NATIVE_UI_MEDIA`
points to a locally permitted six-second fixture.

The test starts Avalonia's real Win32 STA desktop lifetime. It verifies
the D3D11 swapchain, persistent GPU child HWND, six NativeControlHost
detach/attach cycles (including deferred proxy teardown), child pixel
resizing, epoch-advancing seek, EOF, replay and native shutdown before
HWND destruction. Ordinary managed unit CI requires no native DLL.

This is NOT proof of first-frame pixel correctness, hardware DPI,
physical GPU device removal, long-duration stability or audio endpoint
migration; those remain explicit release gates.

## Evidence / sign-off

For each case capture: date, device model and driver, Windows build, DPI scale,
media container+codec+duration, reproduction steps, result, crash dump/trace ID.
For A08/A09 use **匯出診斷** after reproducing the failure to
capture the last 128 session events, overwritten-event count, and raw
HRESULT/removed-reason fields. Also capture `Lapis.D3D11`/`Lapis.VideoRender`
debug-output lines if more detailed driver evidence is needed; a clean Windows
CI run cannot substitute for a controlled real-driver fault.
Only mark as passed when performed on real Windows hardware. CI green is not
a substitute for A01/A07/A08/A09/A10. The Avalonia GPU HWND lifetime now avoids
directly destroying the Native swap-chain HWND, but A07 is still an explicit
release blocker until independent GPU/DPI hardware runs are completed.


## Automated post-warmup resource growth gate

The Windows CTest `windows_native_resource_growth_quick` executes **20**
real FFmpeg/D3D11 cycles (alternating natural EOF and early Close), with the
first **four** cycles warming codec and graphics allocations. The dedicated,
platform-independent `soak_resource_budget_contract` CTest deliberately
simulates resource peaks, drops and threshold violations to verify that the
acceptance gate itself works. Both tests run on every relevant Windows CI run.

**Every completed cycle is sampled after `Close`**, even when the JSON
sample trace is capped at 128 entries. The gate compares the maximum
*post-warmup, post-Close* count against the fourth-cycle baseline. It does
not merely compare the last cycle, which could miss an earlier resource
spike that later settled. The post-warmup envelope has independent maxima
for Handle, GDI, USER, Private Bytes and Working Set; the latter remains
observational only. The CI budgets are intentionally loose: **64 handles,
32 GDI objects, 32 USER objects, 128 MiB Private Bytes**.

The JSON includes `growthComparison: "post_close_peak_after_warmup"`,
`postWarmup`, `postWarmupPeak`, `postWarmupSamples`, `baseline`,
`last`, `peak` and the configured limits. If a selected resource exceeds
its budget, the runner writes an explicit `*_growth_limit_exceeded`
failure and exits nonzero. Supplying a nonzero CLI limit enables checking
of that metric; omitted limits are ignored. A missing warmup or missing
post-warmup sample also fails when checking is enabled.

For an extended local Windows run, use a clear, locally permitted sample:

```powershell
.\build\native\Release\Lapis.PlaybackSoak.exe `
  --media "C:\Samples\short-clear-video.mp4" `
  --cycles 2000 --warmup-cycles 20 --mode mixed `
  --max-handle-growth 64 --max-gdi-growth 32 `
  --max-user-growth 32 --max-private-growth-mib 128 `
  --json ".\lapis-resource-growth.json"
```

The thresholds are *operator-specified process-wide acceptance budgets*,
not hard-coded proof of leak freedom. This only samples **after** a full
Open/Play/Close cycle; it does not catch a within-cycle transient that
vanishes before Close, and cannot measure VRAM lifetime or real device
removal. Driver caching, WARP behavior, allocator retention and background
activity can change these numbers. Repeat on actual hardware and inspect
trends before certifying A01.
