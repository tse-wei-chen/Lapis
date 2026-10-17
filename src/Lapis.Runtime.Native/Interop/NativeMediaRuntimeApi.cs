using System.Runtime.InteropServices;

namespace Lapis.Runtime.Native.Interop;

internal interface INativeMediaRuntimeApi
{
    uint GetAbiVersion();
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
}

internal sealed class PInvokeNativeMediaRuntimeApi : INativeMediaRuntimeApi
{
    internal static PInvokeNativeMediaRuntimeApi Instance { get; } = new();

    private PInvokeNativeMediaRuntimeApi() {}

    public uint GetAbiVersion() => NativeMethods.RuntimeGetAbiVersion();

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
}
