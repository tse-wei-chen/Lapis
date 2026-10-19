using System.Runtime.InteropServices;

namespace Lapis.Runtime.Native.Interop;

internal interface INativeMediaRuntimeApi
{
    uint GetAbiVersion();
    uint GetDiagnosticsApiVersion();
    NativeResult SessionReadDiagnosticEvents(
        nint session, ulong sinceSequence, NativeDiagnosticEvent[] events,
        out uint count, out ulong nextSequence, out ulong dropped);
    NativeResult RuntimeCreate(out nint runtime);
    void RuntimeDestroy(nint runtime);
    NativeResult SessionCreate(
        nint runtime,
        NativePlaybackSessionId sessionId,
        out nint session);
    void SessionDestroy(nint session);
    NativeResult SessionOpen(
        nint session,
        NativeMediaLocatorKind kind,
        string value,
        NativePlaybackEpoch epoch,
        out NativeOpenResult result);
    NativeResult SessionPlay(nint session, NativePlaybackEpoch epoch);
    NativeResult SessionPause(nint session, NativePlaybackEpoch epoch);
    NativeResult SessionSeek(
        nint session,
        NativeMediaTime position,
        NativePlaybackEpoch epoch);
    NativeResult SessionClose(nint session, NativePlaybackEpoch epoch);
    NativeResult SessionGetSnapshot(nint session, out NativePlaybackSnapshot snapshot);
    NativeResult SessionSetVideoOutputSurface(
        nint session,
        NativeVideoOutputSurface surface,
        NativePlaybackEpoch epoch,
        out NativeVideoOutputSurfaceBinding binding);
}

internal sealed class PInvokeNativeMediaRuntimeApi : INativeMediaRuntimeApi
{
    internal static PInvokeNativeMediaRuntimeApi Instance { get; } = new();

    private PInvokeNativeMediaRuntimeApi() {}

    public uint GetAbiVersion() => NativeMethods.RuntimeGetAbiVersion();

    public uint GetDiagnosticsApiVersion() =>
        NativeMethods.RuntimeGetDiagnosticsApiVersion();

    public NativeResult SessionReadDiagnosticEvents(
        nint session, ulong sinceSequence, NativeDiagnosticEvent[] events,
        out uint count, out ulong nextSequence, out ulong dropped)
    {
        // Allocate only for an explicit report export, never per video frame.
        // The native API receives a stable, writable blittable buffer.
        var pointer = Marshal.AllocHGlobal(
            checked(events.Length * Marshal.SizeOf<NativeDiagnosticEvent>()));
        try
        {
            var result = NativeMethods.SessionReadDiagnosticEvents(
                session, sinceSequence, pointer, checked((uint)events.Length),
                out count, out nextSequence, out dropped);
            if (count > events.Length)
            {
                throw new InvalidOperationException(
                    "Native diagnostics returned more records than capacity.");
            }
            if (result == NativeResult.Ok)
            {
                var size = Marshal.SizeOf<NativeDiagnosticEvent>();
                for (var index = 0; index < count; index++)
                {
                    events[index] = Marshal.PtrToStructure<NativeDiagnosticEvent>(
                        pointer + (int)index * size);
                }
            }
            return result;
        }
        finally
        {
            Marshal.FreeHGlobal(pointer);
        }
    }

    public NativeResult RuntimeCreate(out nint runtime) =>
        NativeMethods.RuntimeCreate(out runtime);

    public void RuntimeDestroy(nint runtime) => NativeMethods.RuntimeDestroy(runtime);

    public NativeResult SessionCreate(
        nint runtime,
        NativePlaybackSessionId sessionId,
        out nint session) =>
        NativeMethods.SessionCreate(runtime, sessionId, out session);

    public void SessionDestroy(nint session) => NativeMethods.SessionDestroy(session);

    public NativeResult SessionOpen(
        nint session,
        NativeMediaLocatorKind kind,
        string value,
        NativePlaybackEpoch epoch,
        out NativeOpenResult result)
    {
        ArgumentNullException.ThrowIfNull(value);

        var valueUtf8 = Marshal.StringToCoTaskMemUTF8(value);
        try
        {
            var locator = new NativeMediaLocator
            {
                Kind = kind,
                ValueUtf8 = valueUtf8
            };
            return NativeMethods.SessionOpen(session, locator, epoch, out result);
        }
        finally
        {
            Marshal.FreeCoTaskMem(valueUtf8);
        }
    }

    public NativeResult SessionPlay(nint session, NativePlaybackEpoch epoch) =>
        NativeMethods.SessionPlay(session, epoch);

    public NativeResult SessionPause(nint session, NativePlaybackEpoch epoch) =>
        NativeMethods.SessionPause(session, epoch);

    public NativeResult SessionSeek(
        nint session,
        NativeMediaTime position,
        NativePlaybackEpoch epoch) =>
        NativeMethods.SessionSeek(session, position, epoch);

    public NativeResult SessionClose(nint session, NativePlaybackEpoch epoch) =>
        NativeMethods.SessionClose(session, epoch);

    public NativeResult SessionGetSnapshot(
        nint session, out NativePlaybackSnapshot snapshot) =>
        NativeMethods.SessionGetSnapshot(session, out snapshot);

    public NativeResult SessionSetVideoOutputSurface(
        nint session,
        NativeVideoOutputSurface surface,
        NativePlaybackEpoch epoch,
        out NativeVideoOutputSurfaceBinding binding) =>
        NativeMethods.SessionSetVideoOutputSurface(session, surface, epoch, out binding);
}
