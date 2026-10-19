using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Lapis.Desktop.WinForms;

/// <summary>
/// A WinForms layout control is NOT a stable D3D swap-chain target: changing
/// DPI, parent or styles can recreate its HWND without an async pre-destroy
/// callback. The renderer instead borrows a separately-owned child HWND.
/// Before the Panel receives default WM_DESTROY processing, its child is moved
/// to an independent hidden parking window, then reattached on HandleCreated.
/// All Win32 window mutations are on the UI thread; D3D remains native.
/// </summary>
internal sealed class VideoSurfacePanel : Panel
{
    private const int WmDestroy = 0x0002;
    private const int WsChild = 0x40000000;
    private const int WsVisible = 0x10000000;
    private const int WsDisabled = 0x08000000;
    private const int WsClipChildren = 0x02000000;
    private const int WsClipSiblings = 0x04000000;
    private const int WsPopup = unchecked((int)0x80000000);
    private const uint SwpNoZOrder = 0x0004;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpShowWindow = 0x0040;

    private NativeWindow? _parkingWindow;
    private NativeWindow? _outputWindow;
    private bool _disposed;
    private bool _parkFailed;

    public int ParentHandleRecreations { get; private set; }

    protected override CreateParams CreateParams
    {
        get
        {
            var parameters = base.CreateParams;
            parameters.Style |= WsClipChildren;
            return parameters;
        }
    }

    /// <summary>Get the persistent HWND to pass to the native surface port.</summary>
    public nint GetOutputHandle()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!IsHandleCreated)
        {
            _ = Handle; // UI thread only
        }

        if (_outputWindow is not null)
        {
            if (_parkFailed || !IsWindow(_outputWindow.Handle))
            {
                throw new InvalidOperationException(
                    "The video output HWND could not survive its parent transition.");
            }
            return _outputWindow.Handle;
        }

        var parking = new NativeWindow();
        var output = new NativeWindow();
        try
        {
            // The parking parent MUST NOT be a descendant/owned popup of the
            // Form or Panel. It must outlive all WinForms handle recreation.
            parking.CreateHandle(new CreateParams
            {
                Caption = "Lapis video output parking",
                Style = WsPopup,
                Width = 1,
                Height = 1
            });
            output.CreateHandle(new CreateParams
            {
                Parent = Handle,
                // Disabled child keeps mouse/keyboard routed to the panel
                // while still accepting D3D/DXGI rendering.
                Style = WsChild | WsVisible | WsDisabled | WsClipSiblings,
                Width = Math.Max(1, ClientSize.Width),
                Height = Math.Max(1, ClientSize.Height)
            });
            _parkingWindow = parking;
            _outputWindow = output;
            ResizeOutput();
            return output.Handle;
        }
        catch
        {
            output.DestroyHandle();
            parking.DestroyHandle();
            throw;
        }
    }

    /// <summary>
    /// Test-only synchronous container recreation: the *video* HWND remains
    /// stable so no GPU thread rendezvous can deadlock the UI dispatcher.
    /// </summary>
    public void RecreateForDiagnostics() => RecreateHandle();

    protected override void WndProc(ref Message message)
    {
        // WM_DESTROY is delivered to a parent before Windows destroys children.
        // Park synchronously using User32 only; NEVER wait for a render-thread
        // ACK here because Present/ResizeBuffers can depend on the UI thread.
        if (message.Msg == WmDestroy && _outputWindow is not null && !_disposed)
        {
            ParkOutputBeforeParentDestruction();
        }
        base.WndProc(ref message);
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (_outputWindow is not null && !_disposed && !_parkFailed)
        {
            ParentHandleRecreations++;
            AttachOutput(Handle);
            ResizeOutput();
        }
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        if (_outputWindow is not null && !_parkFailed)
        {
            ResizeOutput();
        }
    }

    private void ParkOutputBeforeParentDestruction()
    {
        if (_outputWindow is null || _parkingWindow is null ||
            GetParent(_outputWindow.Handle) == _parkingWindow.Handle)
        {
            return;
        }

        if (SetParent(_outputWindow.Handle, _parkingWindow.Handle) == 0)
        {
            // A failed SetParent means Win32 may destroy the output along with
            // the panel. Do not silently rebind the same invalid HWND later.
            _parkFailed = true;
            Trace.TraceError(
                "Lapis VideoSurfacePanel: parking failed (Win32 {0}).",
                Marshal.GetLastWin32Error());
        }
    }

    private void AttachOutput(nint newParent)
    {
        if (_outputWindow is null)
        {
            return;
        }

        if (GetParent(_outputWindow.Handle) != newParent &&
            SetParent(_outputWindow.Handle, newParent) == 0)
        {
            _parkFailed = true;
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Could not reparent the persistent video output HWND.");
        }
    }

    private void ResizeOutput()
    {
        if (_outputWindow is not null &&
            !SetWindowPos(
                _outputWindow.Handle, 0, 0, 0,
                Math.Max(1, ClientSize.Width),
                Math.Max(1, ClientSize.Height),
                SwpNoZOrder | SwpNoActivate | SwpShowWindow))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Could not resize the video output HWND.");
        }
    }

    /// <summary>
    /// Called only after the native host has joined its renderer and released
    /// the swap chain. The container HWND can then safely outlive or be closed.
    /// </summary>
    public void ReleaseOutputAfterRuntimeDisposed()
    {
        _outputWindow?.DestroyHandle();
        _outputWindow = null;
        _parkingWindow?.DestroyHandle();
        _parkingWindow = null;
        _parkFailed = false;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && !_disposed)
        {
            _disposed = true;
            ReleaseOutputAfterRuntimeDisposed();
        }
        base.Dispose(disposing);
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern nint GetParent(nint hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern nint SetParent(nint child, nint newParent);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        nint hwnd, nint insertAfter, int x, int y, int width, int height,
        uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint hwnd);
}
