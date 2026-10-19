using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform;

namespace Lapis.Desktop.Avalonia.Controls;

/// <summary>
/// The only Win32 dependency in the Avalonia UI. The D3D11 renderer borrows a
/// child HWND under an Avalonia-owned proxy window. Avalonia may freely
/// park/rehost its proxy; only when that proxy is destroyed do we park the GPU
/// child under an independent hidden HWND. Never synchronously wait for a
/// Native render-thread ACK on the UI thread.
/// </summary>
public sealed class WindowsVideoSurface : NativeControlHost
{
    private const uint WsPopup = 0x80000000;
    private const uint WsChild = 0x40000000;
    private const uint WsVisible = 0x10000000;
    private const uint WsDisabled = 0x08000000;
    private const uint WsClipSiblings = 0x04000000;
    private const uint SwpNoZOrder = 0x0004;
    private const uint SwpNoActivate = 0x0010;

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

        // Avalonia may reattach the SAME NativeControlHost attachment without
        // calling CreateNativeControlCore again. Never hand our GPU HWND
        // directly to Avalonia. Let Avalonia own a lightweight proxy child
        // instead; it may move/reparent that proxy and our GPU child follows.
        // The GPU child itself is borrowed only by DesktopPlaybackHost.
        var proxy = base.CreateNativeControlCore(parent);
        try
        {
            if (_output != 0)
            {
                // A deferred detach may have destroyed an earlier proxy.
                // The GPU child was parked first in DestroyNativeControlCore.
                if (!IsWindow(_output) || SetParent(_output, proxy.Handle) == 0)
                {
                    _parkFailed = true;
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "The persistent video HWND could not be reattached.");
                }
                return proxy;
            }

            // Parking HWND is a hidden top-level window with an independent
            // lifetime. It cannot be owned by the Avalonia top-level Window.
            _parking = CreateWindowEx(
                0, "STATIC", "Lapis GPU HWND parking", WsPopup,
                0, 0, 1, 1, 0, 0, 0, 0);
            if (_parking == 0)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not create video parking window.");
            }
            _output = CreateWindowEx(
                0, "STATIC", "",
                WsChild | WsVisible | WsDisabled | WsClipSiblings,
                0, 0, 1, 1, proxy.Handle, 0, 0, 0);
            if (_output == 0)
            {
                var error = Marshal.GetLastWin32Error();
                DestroyWindow(_parking);
                _parking = 0;
                throw new Win32Exception(error,
                    "Could not create the D3D11 child HWND.");
            }
            return proxy;
        }
        catch
        {
            base.DestroyNativeControlCore(proxy);
            throw;
        }
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var arranged = base.ArrangeOverride(finalSize);

        // Avalonia sizes only its own proxy. The actual GPU child HWND is
        // independently created, so keep its Win32 client area synchronized
        // with the Avalonia layout's physical pixel size (including DPI).
        if (!_retired && !_parkFailed && _output != 0 &&
            double.IsFinite(arranged.Width) && double.IsFinite(arranged.Height))
        {
            var scale = TopLevel.GetTopLevel(this)?.RenderScaling ?? 1.0;
            var width = (int)Math.Clamp(
                Math.Round(arranged.Width * scale), 1, 8192);
            var height = (int)Math.Clamp(
                Math.Round(arranged.Height * scale), 1, 8192);
            if (!SetWindowPos(_output, 0, 0, 0, width, height,
                    SwpNoZOrder | SwpNoActivate))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not resize the D3D11 child HWND.");
            }
        }

        return arranged;
    }

    protected override void DestroyNativeControlCore(IPlatformHandle control)
    {
        if (!_retired && _output != 0)
        {
            // Only a deferred Avalonia destruction of the PROXY requires
            // parking our child. A quick detach/attach can reuse its proxy,
            // so CreateNativeControlCore might not be called at all.
            if (!IsWindow(_output) || _parking == 0 ||
                SetParent(_output, _parking) == 0)
            {
                _parkFailed = true;
                Trace.TraceError("Lapis: video HWND parking failed (Win32 {0}).",
                    Marshal.GetLastWin32Error());
            }
        }

        // Avalonia destroys its own proxy HWND, never the D3D11 child.
        base.DestroyNativeControlCore(control);
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
    private static extern bool SetWindowPos(
        nint handle, nint insertAfter, int x, int y,
        int width, int height, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyWindow(nint handle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint handle);
}
