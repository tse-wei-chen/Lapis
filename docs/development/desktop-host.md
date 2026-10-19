# Running the first Lapis desktop video host

This slice adds a runnable Windows **WinForms shell** with a reusable
UI-framework-neutral desktop playback coordinator. It is a development host, not
a packaged PowerDVD replacement yet.

## Requirements

- Windows 10/11 with the .NET **10** SDK and Windows Desktop runtime
- Visual Studio C++ toolchain, Windows SDK, CMake
- vcpkg using the native project's pinned manifest/triplet
- An accessible D3D11 adapter; WARP software fallback is supported
- A local media file readable by the existing FFmpeg source adapter

## Build and run

From repository root in a Windows PowerShell with vcpkg configured:

```powershell
cmake -S native/MediaRuntime -B build/native -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_OVERLAY_TRIPLETS=$PWD/native/MediaRuntime/vcpkg-triplets" `
  "-DVCPKG_TARGET_TRIPLET=x64-windows-release"
cmake --build build/native --config Release
ctest --test-dir build/native -C Release --output-on-failure
dotnet restore Lapis.slnx
dotnet build Lapis.slnx -c Release --no-restore
dotnet test Lapis.slnx -c Release --no-build
dotnet run --project src/Lapis.Desktop.WinForms -c Release --no-build
```

The WinForms project copies the built `Lapis.MediaRuntime.dll` from
`build/native/Release` beside its executable **when the file exists**. The
managed solution builds and tests without requiring a native DLL in CI. If native
FFmpeg dependencies are configured as dynamic libraries instead of the pinned
static triplet, those DLLs must also be deployed next to the executable.

## Desktop UI/UX foundation

The visual player is now a dedicated `PlayerChrome` WinForms UserControl
composed by `PlayerForm`. It remains a **presentation adapter**, not a
media pipeline: playback commands still flow through
`DesktopPlaybackHost`, `PlaybackSession` and the stable native C ABI.
This separation allows future WinUI/other frontends to reuse the same
non-UI application and media contracts.

Visual structure (desktop landscape, dark theme):

- **Header:** Lapis identity, current media filename, native Open button and
  an accurately labelled `AI 雙語字幕 · 規劃中` status. The AI indicator is
  informational only: live translation/subtitles are not implemented yet.
- **Stage:** the existing persistent D3D11 output HWND behind a first-run
  empty-state overlay; both the idle surface and playing surface accept local
  file drag-and-drop. Overlay visibility does not change the GPU lease.
- **Playback deck:** current/total time, seek slider, backward 10 seconds,
  Play/Pause/Replay, forward 10 seconds, Stop, absolute-time Seek and full
  screen. Buttons disable correctly when no media is seekable.
- **Status bar:** human-readable Traditional Chinese transport/failure text
  and on-demand diagnostic JSON export.

Keyboard actions: `Ctrl+O` open, `Space` Play/Pause/Replay,
`←` / `→` seek ±10s with 0/duration clamping, `F11` fullscreen,
`Esc` exit fullscreen and `Ctrl+Shift+H` recreate Panel HWND for
diagnostics. Shortcuts avoid stealing numeric input focus. Fullscreen hides
the UI chrome through layout changes but keeps the same `VideoSurfacePanel`
and native HWND owner. All graphics object lifetime guarantees remain
unchanged. The initial WinForms UX targets keyboard/mouse landscape use;
touch-optimized, dynamic subtitle track controls and the AI sidecar need
separate implementation after their application-level capabilities exist.

The `PlayerChromeTests` Windows STA regression checks first-run and
now-playing state, timeline/status updates, fullscreen visual switching,
accessibility names and stable output HWND ownership. The pre-existing
Native WinForms acceptance continues to exercise FFmpeg/D3D11 with the
actual `VideoSurfacePanel`, independent of the new layout.

## YouTube-inspired focused watching

This iteration prioritizes the **watching interaction** rather than copying
the YouTube brand or surrounding recommendation feed:

- The `PlaybackSeekBar` renders a slim red played range, hover timestamp
  and white scrub thumb. It supports click/drag/release and keyboard seek.
  Only real `PlaybackSnapshot` duration and position are used; thumbnails,
  buffered progress and chapters are not fabricated.
- **Theater mode** (`T` or the `▣ 劇院` control) removes the header and
  status strip and expands the video stage. **Fullscreen** (`F`/`F11`,
  `Esc` to restore) preserves the native output HWND; in either immersive
  mode, controls disappear after three seconds of active playback inactivity
  and return on pointer movement, pause, seek or transport interaction.
  In the standard windowed layout controls stay visible.
- Clicking the video toggles Play/Pause (after the system double-click
  interval). Double-clicking enters or leaves fullscreen without treating
  the first click as Pause. `J`/`L` or left/right skip 10 seconds,
  `K` or Space toggles play, and `0` returns to the beginning.
  Numeric input keeps its own focus semantics.
- Controls are organized as a clean sibling strip immediately beneath
  the Win32 D3D11 video HWND, **not** a WinForms overlay painted over that
  GPU-owned window. This respects native HWND airspace/z-order and prevents
  presenting a deceptive overlay that vanishes behind the SwapChain.
- `PlayerChrome` exposes the display-only viewing modes; `PlayerForm`
  owns idle/pointer/click policies; `DesktopPlaybackHost` still serializes
  every Native command. There are no new C ABI exports or fake volume,
  subtitles, playback-speed or buffer UI controls.

Tests include WinForms STA viewing-mode and persistent HWND checks,
timeline render/progress bounds and the existing real Native
FFmpeg/D3D11 playback acceptance. A real video-thumbnail provider and
subtitle/AI sidecar are separate future capabilities, not part of this UI
milestone.

## User interaction

Open a local `.mkv`, `.mp4`, `.m2ts`, `.ts`, `.avi`, `.mov`, `.webm`
or similar FFmpeg-supported file through **開啟影片**, Ctrl+O, or drag and drop.
The host opens the media through the playback actor, binds its HWND through
`IVideoOutputSurfaceBinding`, then starts playback. A separate optional
`IPlaybackRuntimeObserver` polls the existing native C ABI snapshot away from
the UI thread, and only matching session/epoch updates reach the actor state.
The seek rail uses native position and open-result duration; clicking or
releasing its thumb seeks to a proportional media timestamp.
The actor's bounded observation channel is followed by a coalescing WinForms
dispatch gate: at most one UI refresh is queued at a time, with one follow-up
when observations arrive during painting. The UI reads the actor's **current**
snapshot at dispatch time rather than rendering a captured pre-Seek value.
A stalled UI therefore does not accumulate an unbounded BeginInvoke backlog,
and old playback epochs cannot repaint newer command results. Space or K toggles pause.
**跳至** seeks to an absolute time in seconds. Playback controls also offer ±10s jumps and F11 fullscreen. **停止** closes the media
generation and detaches the output target. After natural **Ended**, pressing
**重播** (or Space) reopens the same file with a fresh playback generation.
Opening another file likewise retires the previous pipeline.

The Native D3D11 swap chain now targets an independently owned child HWND
rather than the WinForms video Panel HWND. When Windows/WinForms destroys the
Panel (including normal PMv2 handle recreation), its `WM_DESTROY` handler
moves the video HWND to a hidden top-level parking parent **before** Windows
recursively destroys child windows. The video HWND is then reparented to the
new Panel and resized in pixel coordinates; the native swap chain target and
playback epoch do not change. All User32 parenting work stays on the UI thread,
and no native render-thread wait occurs in a Win32 window procedure.

Press **Ctrl+Shift+H** while playing to force a Panel recreation and verify
that the video HWND remains the same. The Windows STA test
`VideoSurfacePanelTests` exercises 40 explicit recreations with no FFmpeg or
D3D device required. This is an architectural stability test, not proof that
all GPU/monitor DPI drivers render correctly after reparenting.

Normal form shutdown cancels video-output HWND destruction until the host has
disposed its session (including render thread + swapchain). The independent
video HWND is destroyed only after native release. A failure in native
teardown cancels Form closing. This contract must be maintained for other
desktop frameworks: never destroy an HWND while the presenter may still use it.

## Architectural boundary

```text
WinForms UI / HWND owner
  -> Lapis.Desktop.Hosting (framework-neutral sequencing)
      -> PlaybackSession actor -> IPlaybackRuntime
      -> IVideoOutputSurfaceBinding
          -> Lapis.Runtime.Native -> stable C ABI v2
              -> PlaybackExecution -> D3D11VideoPresenter
```

The desktop shell does **not** call C ABI or DXGI directly. `DesktopPlaybackHost`
takes independently injectable transport and surface-binding ports. It
serializes Open/Bind/Play, seek and output-window replacement; the binding port
waits off the UI thread for the render-thread ACK.

## Export a bounded diagnostics report

Once the native player has initialized, click **匯出診斷** to save a JSON
report. The export is on demand, **never** part of the media/frame loop.
The report includes UTC capture time, Windows/.NET versions, current transport,
WinForms Panel re-creation count, and up to 128 **session-scoped** native
diagnostic events. No media path, subtitle text, decrypted content, or
transcript is included by the built-in diagnostics event schema. Users should
still inspect reports before sharing them: OS version, playback epoch, and
runtime timing can be identifying when correlated with other records.

Native journal semantics: events are numbered in order, include monotonic
process uptime (not UTC wall time), retain at most the latest **128**, and
survive `Stop`, `Close`, `Open`, and seek within the same session. If the
export is late, `OverwrittenEvents` reports how many older events were
overwritten. The journal is copied without acquiring the render/GPU lock,
without blocking playback for file I/O, and without dynamic allocations
in its event writer.

The existing playback ABI remains **v2**. The optional separately versioned
`lapis_runtime_get_diagnostics_api_version() == 1` plus
`lapis_session_read_diagnostic_events()` provides the diagnostic extension,
with a fixed-width **48-byte** record, an exclusive sequence cursor and
explicit overwrite count. Existing v2 function/structure layouts do not
change. A player with an older DLL lacking these symbols continues to play;
only manual export reports that the diagnostics API is unavailable. This is
the v1 journal/export slice, not a general tracing or crash telemetry service.

## Nonzero seek demux regression

The native churn test exposed a format-specific issue: on small Y4M files
prefetched by FFmpeg's custom AVIO, a nonzero `av_seek_frame()` could report
success even though the next decoded generation immediately reached `Ended`.
The demuxer's Y4M-only seek path now probes the next packet and retains it
for the consumer. If that probe fails, it seeks to zero and scans forward
to the requested timestamp, capped at **5000 packets and 256 MiB of
packet data** (whichever limit is reached first). Packets without a
usable timestamp cannot establish a target location. If recovery fails or
either scan limit is reached, it reports Seek failure rather than silently
publishing false EOF. Indexed containers keep their original native
FFmpeg seek behavior. The packet router and decoder flush remain session
generation responsibilities; the demuxer does not invoke a redundant
`avformat_flush()` after successful Seek.

`ffmpeg_decode_smoke` and `windows_native_playback_y4m_seek_quick`
cover the repaired behavior via decoder and full C ABI / D3D11 routes.
The bounded fallback is not an O(1) indexed seek, and may reject
extremely long unindexed seek distances by design.

## Repeatable Windows soak testing

The native Windows Release target now builds `Lapis.PlaybackSoak.exe`,
a separate console acceptance harness which calls the same C ABI v2 and
native FFmpeg/D3D11 pipeline as the desktop player. Its Win32 output HWND
is explicitly destroyed only after the native session has released DXGI.

`ctest` includes a 4-cycle `windows_native_playback_soak_quick` scenario
and a 2-cycle `windows_native_playback_churn_quick` scenario that
exercises GPU-initialized live HWND replacement and forward/backward Seek
with new playback epochs on alternating 6-second indexed MP4 files. For real stress testing, use a user-owned
short/long sample, run `--cycles 100 --mode mixed` or
`--cycles 100 --mode churn --seek-ms 250 --media-alternate <second-file>`
or `--cycles 1 --mode eof --timeout-seconds 10800`, and inspect the
process handle/Private Bytes/working set peaks and bounded GPU events.
A separate `windows_native_playback_parent_churn_quick` CTest (one
six-second MP4, 16 parent HWND recreations) verifies that a live D3D11
swap-chain survives the same child HWND being parked while its temporary
container HWND is destroyed and rebuilt. This tests the HWND Island
lifetime principle without requiring WinForms or changing monitor DPI;
it is **not** a substitute for A05/A07 physical UI acceptance.
For a longer simulated run, use `--mode parent-churn
--parent-recreates 40 --cycles 1`.
See [real-Windows acceptance](desktop-acceptance.md) for exact PowerShell
examples, available switches and honest hardware-release gates.

## Bounded WASAPI event wait and endpoint loss lifecycle

A WASAPI endpoint may become invalid without posting another audio
notification. The former `WaitForMultipleObjects(INFINITE)` call
could then block forever waiting for audio readiness. The new
`wasapi_event_wait.hpp` internal utility retains event-driven
notifications but checks device padding on a 100 ms timed wait even
without an event, surfacing `DeviceLost` if
`GetCurrentPadding` fails with an invalidation HRESULT. It never
reports Ready merely because a timeout occurred, and Stop/Seek still
signals the stop event immediately. The behavior is covered by
`wasapi_endpoint_loss_event_wait` on Windows CI.

`playback_execution_smoke` now also injects an audio output failure
during PCM Submit and verifies that the transport reports Faulted,
the old output is reset/closed, and a caller-driven close/reopen with
a new PlaybackEpoch chooses a replacement output and reaches clean
EOF. No real WASAPI device or OS default endpoint is modified by CI.
Automatically following default-device changes mid-playback remains
future work; manual hardware A10 validation is still required.

## Deterministic D3D11 device-loss recovery test

Windows CTest `d3d11_live_device_loss_recovery_injection` wraps the real
production `D3D11VideoPresenter` with a test-only `IVideoPresenter`
decorator. The injected `DeviceLost` status flows into the unchanged
production `VideoRenderWorker`, which must drop and recreate **real**
device/SwapChain resources and resume actual GPU presentation. Assertions
cover a transient fault, exhausted retry budget, and recovery-without-a-
following-frame at EOF, including the session diagnostic journal. The
test-only injector is not compiled into the product DLL and no C ABI or
managed Snapshot contract changes. This does not physically remove a GPU
device or replace the A08 hardware acceptance gate.

## GPU / render recovery diagnostics

A native render failure previously collapsed to a generic
`DeviceLost`/`Failed` status. The D3D11 Presenter now publishes an
**atomically readable** `VideoPresenterDiagnostics` record via its optional
`IVideoPresenter::diagnostics()` port, including cumulative successful D3D
device and swap-chain creations, successful `ResizeBuffers` calls, device-loss
events, the last failing HRESULT, the last
`ID3D11Device::GetDeviceRemovedReason()` result, and the failing operation.
`PlaybackExecutionSnapshot.presenter_diagnostics` and the existing
`rendering` counters expose this within native C++ (per generation).

`VideoRenderStats` now also reports per-generation
`recovery_attempts`, `recovery_bind_successes`, and
`recovered_presents`. A successful resource rebind is **not** counted as
recovered playback until a subsequent frame presents successfully.

On Windows, native failures and GPU recovery transitions emit sparse
`OutputDebugStringA` events prefixed `Lapis.D3D11` and
`Lapis.VideoRender`. Capture them with Visual Studio's native Debug
Output window or a Windows debug-output collector while reproducing a
black frame or driver reset. Example event shapes (illustrative, **not**
records from a tested hardware reset):

```text
Lapis.D3D11 | stage=present | hr=0x887A0005 | device_removed_reason=0x887A0006 | device_creations=1 | swap_chains=1 | resizes=4
Lapis.VideoRender | event=device_lost_rebind_start | epoch=4 | attempt=1
Lapis.VideoRender | event=device_lost_rebind_ready | epoch=4 | attempt=1
Lapis.VideoRender | event=recovered_present | epoch=4 | attempt=1
```

A reason of `0x00000000` means the GPU driver reported `S_OK` when
queried; it must **not** be presented as a confirmed device removal.
The cumulative presenter counters remain on the native snapshot. Selected
fault and recovery transitions are additionally retained in the bounded
session journal and can be exported through the optional diagnostics v1 ABI.
The existing playback snapshot C ABI v2 has not changed. This does **not**
measure GPU allocation size, add a persistent background trace file, or
collect crash dumps. For acceptance, export JSON and also capture the native
debug stream if more detailed driver evidence is needed.

## Important current limitations

- This is local-file playback, **not complete BDMV navigation**. Directory
  playlists/BD-J menus and protected commercial discs are not supported yet.
- The output is SDR BGRA8 CPU-upload; GPU decode, HDR color management
  and subtitle overlay are not part of the shell. The video worker now attempts
  up to two render-thread D3D resource rebinds per playback generation after
  a DeviceLost result. Repeated failure still stops rendering and is reported
  as a failed/unavailable presenter; it is not a full cross-adapter recovery
  policy.
- Runtime snapshots are sampled every 150 ms while active, rather than being
  pushed on each frame. The visible position reflects the last observed native
  clock and is not a sample-accurate output-position guarantee. A future event
  channel can replace polling behind the same optional observation port.
- Native video output now has a stable HWND island. Parking in `WM_DESTROY`
  prevents the WinForms Panel from destroying the native presentation target
  during automatic Panel recreation; the subsequent `HandleCreated` reattaches
  it. Native decoder/GPU code does not depend on the WinForms Panel HWND.
  `SetParent` still has DPI-awareness restrictions; the island and its
  parking parent are created on the same PerMonitorV2 UI thread. Unexpected
  `SetParent` failures are logged/marked as invalid, not silently recovered.
  This is **not** multi-monitor hardware acceptance: A07 remains open until
  real 100%/150%/200% monitor transitions prove that the swap chain resumes.
  Form and parent-container destruction paths also require real validation.
- See [physical Windows acceptance matrix](desktop-acceptance.md) before
  claiming a long-running desktop release.
- On native adapter startup errors, build the native Release target and check
  native DLL compatibility (ABI v2) and dependency availability.
- No installer, signed binaries, or redistributable runtime packaging are
  provided in this development milestone.
