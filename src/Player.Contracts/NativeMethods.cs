using System.Runtime.InteropServices;

namespace BdmvPlayer.Contracts;

public enum NativeStatus : int
{
    Ok = 0, InvalidArgument = 1, AbiMismatch = 2, OutOfMemory = 3,
    InvalidState = 4, Unsupported = 5, InternalError = 6,
    Again = 7, BufferTooSmall = 8, EndOfStream = 9, DecodeError = 10
}
public enum TransportState : uint
{
    Closed = 0, Opening = 1, Ready = 2, Playing = 3,
    Paused = 4, Seeking = 5, Ended = 6, Faulted = 7
}

[StructLayout(LayoutKind.Sequential)]
public struct AbiVersion
{
    public uint StructSize;
    public uint Major;
    public uint Minor;
    public uint Patch;
    public static AbiVersion Create() => new() { StructSize = (uint)Marshal.SizeOf<AbiVersion>() };
}

[StructLayout(LayoutKind.Sequential)]
public struct EngineConfig
{
    public uint StructSize;
    public uint Flags;
    public uint VideoBackend;
    public uint AudioBackend;
    public static EngineConfig Create() => new() { StructSize = (uint)Marshal.SizeOf<EngineConfig>() };
}

[StructLayout(LayoutKind.Sequential)]
public struct EngineCapabilities
{
    public uint StructSize;
    public uint TestSource;
    public uint FileSource;
    public uint BluraySource;
    public uint VideoOutput;
    public uint AudioOutput;
    public uint Reserved;
    public static EngineCapabilities Create() => new() { StructSize = (uint)Marshal.SizeOf<EngineCapabilities>() };
}

[StructLayout(LayoutKind.Sequential)]
public struct SessionSnapshot
{
    public uint StructSize;
    public TransportState Transport;
    public ulong SessionId;
    public ulong Epoch;
    public long PositionNanoseconds;
    public uint Navigation;
    public uint Output;
    public uint Ai;
    public uint Reserved;
    public static SessionSnapshot Create() => new() { StructSize = (uint)Marshal.SizeOf<SessionSnapshot>() };
}

[StructLayout(LayoutKind.Sequential)]
public struct DecodedFrameInfo
{
    public uint StructSize;
    public uint Kind;
    public ulong SessionId;
    public ulong Epoch;
    public long PtsNanoseconds;
    public long DurationNanoseconds;
    public uint Width;
    public uint Height;
    public uint SampleRate;
    public uint Channels;
    public ulong RequiredBytes;
    public static DecodedFrameInfo Create() => new() { StructSize = (uint)Marshal.SizeOf<DecodedFrameInfo>() };
}

// Exactly one C ABI boundary. Managed layers never touch FFmpeg pointers or GPU handles.
public static class NativeMethods
{
    private const string RuntimeName = "MediaRuntime";

    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_get_abi_version(ref AbiVersion version);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_engine_create(in EngineConfig config, out nint engine);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void bdmv_engine_destroy(nint engine);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_engine_query_capabilities(nint engine, ref EngineCapabilities caps);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_create(nint engine, out nint session);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void bdmv_session_destroy(nint session);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_open_uri(nint session, [MarshalAs(UnmanagedType.LPUTF8Str)] string uri);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_play(nint session);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_pause(nint session);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_seek(nint session, long positionNs);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_close(nint session);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_read_frame(nint session, ref DecodedFrameInfo frame, nint buffer, ulong capacity);
    [DllImport(RuntimeName, CallingConvention = CallingConvention.Cdecl)]
    public static extern NativeStatus bdmv_session_get_snapshot(nint session, ref SessionSnapshot snapshot);
}
