using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Avalonia.Controls;
using Avalonia.Platform;

namespace Lapis.Desktop.Avalonia.Controls;

/// <summary>
/// The only Win32 dependency in the Avalonia UI. The D3D11 renderer borrows a
/// child HWND, never Avalonia's own window. When Avalonia detaches this host,
/// synchronously park the child under an independent hidden Win32 window.
/// Never synchronously await a Native render-thread ACK on the UI thread.
/// </summary>
public sealed class WindowsVideoSurface : NativeControlHost
{
    private const uint WsPopup = 0x80000000;
    private const uint WsChild = 0x40000000;
    private const uint WsVisible = 0x10000000;
    private const uint WsDisabled = 0x08000000;
    private const uint WsClipSiblings = 0x04000000;

    private nint _parking;
    private nint _output;
    private bool _retired;
    private bool _parkFailed;

    public nint OutputHandle
    {
        get
        {
            if (_retired || _parkFailed || _output == 0 || !IsWindow(_output))
            {
                throw new InvalidOperationException(
                    "The Avalonia video surface is not attached to a valid HWND.");
            }
            return _output;
        }
    }

    protected override IPlatformHandle CreateNativeControlCore(IPlatformHandle parent)
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException(
                "The current Lapis D3D11 output adapter requires Windows.");
        }
        if (_retired || _parkFailed || parent.Handle == 0)
        {
            throw new InvalidOperationException("The video surface cannot be attached.");
        }

        if (_output != 0)
        {
            if (!IsWindow(_output) || SetParent(_output, parent.Handle) == 0)
            {
                _parkFailed = true;
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "The persistent video HWND could not be reattached.");
            }
            return new PlatformHandle(_output, "HWND");
        }

        // Parking HWND is a hidden top-level window with independent lifetime.
        // It must not be a child of the Avalonia top-level Window.
        _parking = CreateWindowEx(
            0, "STATIC", "Lapis GPU HWND parking", WsPopup,
            0, 0, 1, 1, 0, 0, 0, 0);
        if (_parking == 0)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Could not create video parking window.");
        }
        _output = CreateWindowEx(
            0, "STATIC", "", WsChild | WsVisible | WsDisabled | WsClipSiblings,
            0, 0, 1, 1, parent.Handle, 0, 0, 0);
        if (_output == 0)
        {
            var error = Marshal.GetLastWin32Error();
            DestroyWindow(_parking);
            _parking = 0;
            throw new Win32Exception(error, "Could not create the D3D11 child HWND.");
        }
        return new PlatformHandle(_output, "HWND");
    }

    protected override void DestroyNativeControlCore(IPlatformHandle control)
    {
        if (_retired || _output == 0)
        {
            return;
        }

        // Avalonia's host may be recreated on layout/visual-root changes.
        // Keep the D3D11 target alive until the *actor* has finished shutdown.
        if (!IsWindow(_output) || _parking == 0 ||
            SetParent(_output, _parking) == 0)
        {
            _parkFailed = true;
            Trace.TraceError("Lapis: video HWND parking failed (Win32 {0}).",
                Marshal.GetLastWin32Error());
        }
    }

    /// <summary>
    /// Call on the Avalonia UI thread ONLY AFTER DesktopPlaybackHost.DisposeAsync
    /// has released the presenter, renderer and native session. Never during a
    /// NativeControlHost attach/detach callback.
    /// </summary>
    public void ReleaseAfterPlaybackStopped()
    {
        if (_retired) return;
        _retired = true;
        if (_output != 0)
        {
            DestroyWindow(_output);
            _output = 0;
        }
        if (_parking != 0)
        {
            DestroyWindow(_parking);
            _parking = 0;
        }
    }

    [DllImport("user32.dll", EntryPoint = "CreateWindowExW",
        CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint CreateWindowEx(
        uint extendedStyle, string className, string title, uint style,
        int x, int y, int width, int height,
        nint parent, nint menu, nint instance, nint parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern nint SetParent(nint child, nint parent);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyWindow(nint handle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint handle);
}
