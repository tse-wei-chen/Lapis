# Avalonia desktop player — Windows first

**Product UI:** `src/Lapis.Desktop.Avalonia` (Avalonia 11.3.22 with MVVM).
**Legacy verification only:** `src/Lapis.Desktop.WinForms`.

This is a first vertical slice, not a claim of complete PowerDVD parity.
UI/VM can be ported, but Native FFmpeg/D3D11/WASAPI and the Win32 HWND output
remain Windows-specific.

## Run

Build the Native runtime with CMake/vcpkg per the repository README, then:

```powershell
dotnet restore Lapis.slnx
dotnet build Lapis.slnx -c Release --no-restore
dotnet run --project src/Lapis.Desktop.Avalonia -c Release --no-build
```

The managed project copies `build/native/Release/Lapis.MediaRuntime.dll`
beside its executable when it exists; codec dependencies also need to be
on the DLL search path or deployed next to the app. The window can open
without a Native DLL, but playing media cannot.

Use **開啟影片** for local files and **開啟 BDMV** for an unprotected
disc directory. Actual BDMV playback requires the separately validated,
opt-in `LAPIS_ENABLE_LIBBLURAY=ON` native build, and all its runtime DLLs.
BD-J, AACS/BD+, optical drive access, ISO, subtitle overlay and AI
translation are **not implemented** by this shell.

## Safety/ownership boundaries

```text
MainWindow (Avalonia picker + lifetime)
  -> PlayerViewModel (UI state, commands; no HWND or D3D)
    -> IPlayerPlaybackService
      -> NativePlayerPlaybackService (Windows composition)
        -> DesktopPlaybackHost
          -> PlaybackSession Actor
            -> Native MediaRuntime
NativeControlHost -> persistent child HWND borrowed by DesktopPlaybackHost
```

The video host parks its child HWND under an independent hidden top-level
window during Avalonia attach/detach, rather than destroying a surface the
D3D11 presenter may still own. It is destroyed **only after** the host's
native shutdown/worker join completes. Never wait synchronously for native
render ACK inside Avalonia UI/native host callbacks.

NativeControlHost is a real HWND: Avalonia controls **cannot reliably
overlay it**, including styled translucent subtitle/mouse menus. Put controls
outside the video rectangle until a proper GPU compositor/shared surface
exists. This is explicitly a blocker for final AI subtitles/BD-J UI.

## Tests

`tests/Lapis.Desktop.Avalonia.Tests` covers local BDMV directory matching,
typed locator forwarding and ViewModel errors without real Native DLLs.
The legacy WinForms Native GPU test remains the only established Windows
integration acceptance until a dedicated Avalonia HWND/first-frame test and
hardware verification are added.
