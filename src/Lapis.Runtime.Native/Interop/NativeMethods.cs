using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Lapis.Runtime.Native.Interop;

internal static partial class NativeMethods
{
    private const string LibraryName = "Lapis.MediaRuntime";

    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_get_abi_version")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial uint RuntimeGetAbiVersion();

    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_create")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult RuntimeCreate(out nint runtime);

    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_destroy")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void RuntimeDestroy(nint runtime);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_create")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionCreate(
        nint runtime,
        NativePlaybackSessionId sessionId,
        out nint session);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_destroy")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void SessionDestroy(nint session);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_open")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionOpen(
        nint session,
        in NativeMediaLocator locator,
        NativePlaybackEpoch epoch,
        out NativeOpenResult result);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_play")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionPlay(
        nint session,
        NativePlaybackEpoch epoch);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_pause")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionPause(
        nint session,
        NativePlaybackEpoch epoch);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_seek")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionSeek(
        nint session,
        NativeMediaTime position,
        NativePlaybackEpoch epoch);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_close")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionClose(
        nint session,
        NativePlaybackEpoch epoch);
}
