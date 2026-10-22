namespace Lapis.Application.Runtime;

/// <summary>
/// Presentation target kind, independent of decoded-frame backing storage.
/// NativeHandle is an opaque host-owned pointer-sized value, not an owned handle.
/// </summary>
public enum VideoOutputSurfaceKind
{
    None = 0,
    Win32Window = 1,
    X11Window = 2
}

public readonly record struct VideoOutputSurface(
    VideoOutputSurfaceKind Kind,
    nint NativeHandle)
{
    public static VideoOutputSurface None => default;

    public static VideoOutputSurface ForX11Window(uint xid) =>
        xid != 0
            ? new VideoOutputSurface(VideoOutputSurfaceKind.X11Window, (nint)xid)
            : throw new ArgumentException("An X11 window XID must be non-zero.", nameof(xid));

    public static VideoOutputSurface ForWin32Window(nint hwnd) =>
        hwnd != 0
            ? new VideoOutputSurface(VideoOutputSurfaceKind.Win32Window, hwnd)
            : throw new ArgumentException("A Win32 output window must have a non-zero handle.", nameof(hwnd));
}

public enum VideoOutputSurfaceBindingState
{
    Applied,
    Deferred
}

/// <summary>
/// Optional application-side presentation-control port. Not part of
/// IPlaybackRuntime: presentation targets need not exist for headless/IPC playback.
/// The caller must keep a window alive until a later successful replacement,
/// detach, close or disposal. A successful call guarantees the old target is
/// no longer accessed even if the new target was only deferred.
/// Surface changes do not advance playback epoch, but reject stale contexts.
/// </summary>
public interface IVideoOutputSurfaceBinding
{
    ValueTask<VideoOutputSurfaceBindingState> SetVideoOutputSurfaceAsync(
        VideoOutputSurface surface,
        RuntimeCommandContext context,
        CancellationToken cancellationToken);
}
