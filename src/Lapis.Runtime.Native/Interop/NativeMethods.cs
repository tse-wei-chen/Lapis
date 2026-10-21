using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Lapis.Runtime.Native.Interop;

internal static partial class NativeMethods
{
    private const string LibraryName = "Lapis.MediaRuntime";

    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_get_source_features")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial uint RuntimeGetSourceFeatures();

    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_get_diagnostics_api_version")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial uint RuntimeGetDiagnosticsApiVersion();

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_read_diagnostic_events")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionReadDiagnosticEvents(
        nint session,
        ulong sinceSequence,
        nint events,
        uint capacity,
        out uint count,
        out ulong nextSequence,
        out ulong dropped);

    // Optional, version-negotiated text cue extension (not base ABI v2).
    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_get_text_subtitle_cues_api_version")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial uint RuntimeGetTextSubtitleCuesApiVersion();

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_start_text_subtitle_load")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionStartTextSubtitleLoad(
        nint session, NativePlaybackEpoch expectedEpoch, uint streamIndex);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_read_text_subtitle_cues")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial NativeResult SessionReadTextSubtitleCues(
        nint session, NativePlaybackEpoch expectedEpoch, uint startIndex,
        NativeTextSubtitleCueInfo* cues, uint cueCapacity,
        byte* textUtf8, uint textCapacity, out uint count, out uint total,
        out uint requiredTextBytes, out NativeTextSubtitleLoadState state);

    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_get_subtitle_tracks_api_version")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial uint RuntimeGetSubtitleTracksApiVersion();

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_list_subtitle_tracks")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static unsafe partial NativeResult SessionListSubtitleTracks(
        nint session,
        NativePlaybackEpoch expectedEpoch,
        NativeSubtitleTrackInfo* tracks,
        uint capacity,
        out uint count);

    [LibraryImport(LibraryName, EntryPoint = "lapis_runtime_get_navigation_api_version")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial uint RuntimeGetNavigationApiVersion();

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_resolve_chapter")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionResolveChapter(
        nint session, uint chapterId, NativePlaybackEpoch expectedEpoch,
        out NativeMediaTime start);

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

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_set_video_output_surface")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionSetVideoOutputSurface(
        nint session,
        NativeVideoOutputSurface surface,
        NativePlaybackEpoch epoch,
        out NativeVideoOutputSurfaceBinding binding);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_get_snapshot")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionGetSnapshot(
        nint session,
        out NativePlaybackSnapshot snapshot);

    [LibraryImport(LibraryName, EntryPoint = "lapis_session_close")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial NativeResult SessionClose(
        nint session,
        NativePlaybackEpoch epoch);
}
