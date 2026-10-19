using Lapis.Application.Runtime;
using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;
using Lapis.Runtime.Native.Interop;

namespace Lapis.Runtime.Native;

public sealed class NativeMediaRuntimeException : Exception
{
    internal NativeMediaRuntimeException(
        string operation,
        NativeResult result)
        : base($"Native media runtime operation '{operation}' failed with result {(int)result} ({result}).")
    {
        Operation = operation;
        ResultCode = (int)result;
    }

    public string Operation { get; }

    public int ResultCode { get; }
}

/// <summary>
/// In-process adapter from the Application playback-runtime port to the stable
/// native C ABI. One adapter instance binds to exactly one PlaybackSessionId.
/// </summary>
public sealed class NativePlaybackRuntime : IPlaybackRuntime, IVideoOutputSurfaceBinding, IPlaybackRuntimeObserver, IChapterNavigationRuntime
{
    private readonly object _gate = new();
    private readonly INativeMediaRuntimeApi _api;
    private readonly RuntimeLease _runtime;
    private SessionLease? _session;
    private PlaybackSessionId? _boundSessionId;
    private bool _disposed;

    /// <summary>
    /// null = pre-feature-query DLL (unknown), false = file-only build,
    /// true = the libbluray Directory provider was compiled into this DLL.
    /// This does not assert that a protected disc can be opened.
    /// </summary>
    public bool? SupportsBdmvDirectory { get; }

    public NativePlaybackRuntime()
        : this(PInvokeNativeMediaRuntimeApi.Instance) {}

    internal NativePlaybackRuntime(INativeMediaRuntimeApi api)
    {
        _api = api ?? throw new ArgumentNullException(nameof(api));

        var actualAbi = _api.GetAbiVersion();
        if (actualAbi != NativeAbi.Version)
        {
            throw new NotSupportedException(
                $"Lapis.MediaRuntime ABI {actualAbi} is incompatible with managed ABI {NativeAbi.Version}.");
        }

        try
        {
            SupportsBdmvDirectory = (_api.GetSourceFeatures() & 0x2u) != 0;
        }
        catch (EntryPointNotFoundException)
        {
            SupportsBdmvDirectory = null; // Older compatible DLL.
        }

        var result = _api.RuntimeCreate(out var runtime);
        ThrowForResult(result, "runtime_create");
        if (runtime == 0)
        {
            throw new NativeMediaRuntimeException("runtime_create", NativeResult.Internal);
        }

        _runtime = new RuntimeLease(_api, runtime);
    }

    public ValueTask<RuntimeOpenResult> OpenAsync(
        MediaLocator locator,
        RuntimeCommandContext context,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(locator);
        cancellationToken.ThrowIfCancellationRequested();

        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            if (locator.Kind == MediaLocatorKind.Directory &&
                SupportsBdmvDirectory == false)
            {
                throw new NotSupportedException(
                    "目前載入的是一般影片播放版 Lapis.MediaRuntime.dll，未編入 libbluray。 " +
                    "請執行 scripts/build-bdmv-player.ps1，然後啟動 " +
                    "build/Lapis.BdmvPlayer/Lapis.Desktop.Avalonia.exe。 " +
                    "目前程式執行目錄：" + AppContext.BaseDirectory);
            }

            var session = EnsureSession(context.SessionId);
            var kind = ToNativeLocatorKind(locator.Kind);
            var epoch = new NativePlaybackEpoch { Value = context.Epoch.Value };
            var result = _api.SessionOpen(
                session,
                kind,
                locator.Value,
                epoch,
                out var openResult);
            if (result == NativeResult.Unsupported &&
                locator.Kind == MediaLocatorKind.Directory)
            {
                // Generic source rejection can mean a build without the
                // opt-in provider or a rejected disc. Never infer DRM state.
                throw new NotSupportedException(
                    "無法開啟完整 BDMV 光碟。Lapis.MediaRuntime.dll 可能沒有編入 " +
                    "libbluray，或 Native 不接受目前的光碟結構。" +
                    "請使用 scripts/build-bdmv-player.ps1 建立 Blu-ray 版本，" +
                    "並確認播放器載入的是該版本的 DLL。" +
                    "目前只支援可讀取的未受保護 BDMV，不支援 AACS/BD+ 或 BD-J 選單。");
            }
            ThrowForResult(result, "session_open");

            MediaTime? duration = null;
            if (openResult.HasDuration != 0)
            {
                if (openResult.Duration.Nanoseconds < 0)
                {
                    throw new NativeMediaRuntimeException(
                        "session_open.duration",
                        NativeResult.Internal);
                }

                duration = new MediaTime(openResult.Duration.Nanoseconds);
            }

            return ValueTask.FromResult(
                new RuntimeOpenResult(
                    (MediaCapability)openResult.Capabilities,
                    duration));
        }
    }

    public ValueTask PlayAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            var session = EnsureSession(context.SessionId);
            var result = _api.SessionPlay(
                session,
                new NativePlaybackEpoch { Value = context.Epoch.Value });
            ThrowForResult(result, "session_play");
        }

        return ValueTask.CompletedTask;
    }

    public ValueTask PauseAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            var session = EnsureSession(context.SessionId);
            var result = _api.SessionPause(
                session,
                new NativePlaybackEpoch { Value = context.Epoch.Value });
            ThrowForResult(result, "session_pause");
        }

        return ValueTask.CompletedTask;
    }

    public ValueTask<MediaTime> ResolveChapterAsync(
        uint chapterId,
        RuntimeCommandContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            uint version;
            try
            {
                version = _api.GetNavigationApiVersion();
            }
            catch (EntryPointNotFoundException exception)
            {
                throw new NotSupportedException(
                    "The installed native runtime has no chapter-navigation extension.",
                    exception);
            }

            if (version != NativeAbi.NavigationVersion)
            {
                throw new NotSupportedException(
                    $"Native navigation API version {version} is not supported.");
            }

            // Do not create a session just to resolve a chapter: the caller
            // must already have opened a media source in this exact session.
            if (_session is null || _boundSessionId != context.SessionId)
            {
                throw new InvalidOperationException(
                    "Chapter resolution requires the active playback session.");
            }

            NativeResult result;
            NativeMediaTime position;
            try
            {
                result = _api.SessionResolveChapter(
                    _session.Handle,
                    chapterId,
                    new NativePlaybackEpoch { Value = context.Epoch.Value },
                    out position);
            }
            catch (EntryPointNotFoundException exception)
            {
                throw new NotSupportedException(
                    "The installed native runtime cannot resolve chapter locations.",
                    exception);
            }
            if (result == NativeResult.InvalidArgument)
            {
                throw new InvalidOperationException(
                    $"Chapter {chapterId} does not exist in the selected title.");
            }
            ThrowForResult(result, "session_resolve_chapter");
            if (position.Nanoseconds < 0)
            {
                throw new InvalidOperationException(
                    "Native navigation returned a negative chapter time.");
            }
            return ValueTask.FromResult(new MediaTime(position.Nanoseconds));
        }
    }

    public ValueTask SeekAsync(
        MediaTime position,
        RuntimeCommandContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (position.Nanoseconds < 0)
        {
            throw new ArgumentOutOfRangeException(
                nameof(position),
                "Seek position cannot be negative.");
        }

        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            var session = EnsureSession(context.SessionId);
            var result = _api.SessionSeek(
                session,
                new NativeMediaTime { Nanoseconds = position.Nanoseconds },
                new NativePlaybackEpoch { Value = context.Epoch.Value });
            ThrowForResult(result, "session_seek");
        }

        return ValueTask.CompletedTask;
    }

    public ValueTask CloseAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            var session = EnsureSession(context.SessionId);
            var result = _api.SessionClose(
                session,
                new NativePlaybackEpoch { Value = context.Epoch.Value });
            ThrowForResult(result, "session_close");
        }

        return ValueTask.CompletedTask;
    }

    /// <summary>
    /// Bind or detach a presentation target without creating a new playback
    /// epoch. An Applied result acknowledges that the old target is released.
    /// Deferred is safe while no render generation is active.
    /// </summary>
    public ValueTask<VideoOutputSurfaceBindingState> SetVideoOutputSurfaceAsync(
        VideoOutputSurface surface,
        RuntimeCommandContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var nativeSurface = ToNativeVideoOutputSurface(surface);
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            var session = EnsureSession(context.SessionId);
            var result = _api.SessionSetVideoOutputSurface(
                session,
                nativeSurface,
                new NativePlaybackEpoch { Value = context.Epoch.Value },
                out var binding);
            ThrowForResult(result, "session_set_video_output_surface");

            return ValueTask.FromResult(binding switch
            {
                NativeVideoOutputSurfaceBinding.Applied => VideoOutputSurfaceBindingState.Applied,
                NativeVideoOutputSurfaceBinding.Deferred => VideoOutputSurfaceBindingState.Deferred,
                _ => throw new NativeMediaRuntimeException(
                    "session_set_video_output_surface.binding",
                    NativeResult.Internal)
            });
        }
    }

    public ValueTask<VideoOutputSurfaceBindingState> ClearVideoOutputSurfaceAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken) =>
        SetVideoOutputSurfaceAsync(VideoOutputSurface.None, context, cancellationToken);

    /// <summary>
    /// Observes native execution state through the existing C ABI v2 snapshot.
    /// Reads are serialized with native lifetime control; the caller is
    /// responsible for dispatching this synchronous P/Invoke away from UI.
    /// This method never lazily creates a native session just to poll it.
    /// </summary>
    public ValueTask<RuntimePlaybackObservation> ReadSnapshotAsync(
        PlaybackSessionId sessionId,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfDisposed();
            if (_session is null || _boundSessionId != sessionId)
            {
                throw new InvalidOperationException(
                    "Cannot observe a native playback session before it is bound.");
            }

            var result = _api.SessionGetSnapshot(_session.Handle, out var snapshot);
            ThrowForResult(result, "session_get_snapshot");
            if (snapshot.Position.Nanoseconds < 0)
            {
                throw new NativeMediaRuntimeException(
                    "session_get_snapshot.position", NativeResult.Internal);
            }
            var transport = snapshot.Transport switch
            {
                NativeTransportState.Closed => TransportState.Closed,
                NativeTransportState.Opening => TransportState.Opening,
                NativeTransportState.Ready => TransportState.Ready,
                NativeTransportState.Playing => TransportState.Playing,
                NativeTransportState.Paused => TransportState.Paused,
                NativeTransportState.Seeking => TransportState.Seeking,
                NativeTransportState.Ended => TransportState.Ended,
                NativeTransportState.Faulted => TransportState.Faulted,
                _ => throw new NativeMediaRuntimeException(
                    "session_get_snapshot.transport", NativeResult.Internal)
            };
            return ValueTask.FromResult(new RuntimePlaybackObservation(
                new PlaybackSessionId(new Guid(snapshot.SessionId.ToByteArray())),
                new PlaybackEpoch(snapshot.Epoch.Value),
                new MediaTime(snapshot.Position.Nanoseconds),
                transport));
        }
    }

    /// <summary>
    /// Explicit on-demand diagnostics export. This optional API is separately
    /// versioned from playback ABI v2 and is not used in the frame loop.
    /// It captures the most recent 128 events (with overwrite accounting)
    /// without collecting the local media path or changing playback state.
    /// </summary>
    public NativeDiagnosticsReport CaptureDiagnostics()
    {
        lock (_gate)
        {
            ThrowIfDisposed();
            uint version;
            try
            {
                version = _api.GetDiagnosticsApiVersion();
            }
            catch (EntryPointNotFoundException exception)
            {
                throw new NotSupportedException(
                    "The installed native DLL does not provide diagnostics v1.",
                    exception);
            }
            if (version != NativeAbi.DiagnosticsVersion)
            {
                throw new NotSupportedException(
                    $"Native diagnostics API {version} is not supported.");
            }

            if (_session is null)
            {
                return new NativeDiagnosticsReport([], 0);
            }

            var native = new NativeDiagnosticEvent[NativeAbi.DiagnosticRingCapacity];
            NativeResult result;
            uint count;
            ulong next;
            ulong dropped;
            try
            {
                result = _api.SessionReadDiagnosticEvents(
                    _session.Handle, 0, native, out count, out next, out dropped);
            }
            catch (EntryPointNotFoundException exception)
            {
                throw new NotSupportedException(
                    "The installed native DLL is missing diagnostics v1 exports.",
                    exception);
            }
            ThrowForResult(result, "session_read_diagnostic_events");
            if (count > native.Length)
            {
                throw new NativeMediaRuntimeException(
                    "session_read_diagnostic_events.count", NativeResult.Internal);
            }
            var events = new List<NativeDiagnosticsEvent>((int)count);
            for (var index = 0; index < count; index++)
            {
                var entry = native[index];
                events.Add(new NativeDiagnosticsEvent(
                    entry.Sequence, entry.Epoch,
                    entry.MonotonicMilliseconds,
                    entry.Kind, entry.FailureStage,
                    $"0x{entry.Hresult:X8}",
                    $"0x{entry.RemovedReason:X8}",
                    entry.Detail));
            }
            return new NativeDiagnosticsReport(events, dropped);
        }
    }

    public ValueTask DisposeAsync()
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return ValueTask.CompletedTask;
            }

            _disposed = true;
            try
            {
                _session?.Dispose();
                _session = null;
            }
            finally
            {
                _runtime.Dispose();
            }
        }

        return ValueTask.CompletedTask;
    }

    private nint EnsureSession(PlaybackSessionId sessionId)
    {
        if (_session is not null)
        {
            if (_boundSessionId != sessionId)
            {
                throw new InvalidOperationException(
                    "A NativePlaybackRuntime instance cannot be rebound to another PlaybackSessionId.");
            }

            return _session.Handle;
        }

        var nativeId = NativePlaybackSessionId.From(sessionId);
        var result = _api.SessionCreate(_runtime.Handle, nativeId, out var session);
        ThrowForResult(result, "session_create");
        if (session == 0)
        {
            throw new NativeMediaRuntimeException("session_create", NativeResult.Internal);
        }

        _session = new SessionLease(_api, session);
        _boundSessionId = sessionId;
        return session;
    }

    private static NativeVideoOutputSurface ToNativeVideoOutputSurface(VideoOutputSurface surface) =>
        surface.Kind switch
        {
            VideoOutputSurfaceKind.None when surface.NativeHandle == 0 =>
                new NativeVideoOutputSurface
                {
                    Kind = NativeVideoOutputSurfaceKind.None,
                    NativeHandle = 0
                },
            VideoOutputSurfaceKind.Win32Window when surface.NativeHandle != 0 =>
                new NativeVideoOutputSurface
                {
                    Kind = NativeVideoOutputSurfaceKind.Win32Window,
                    NativeHandle = (nuint)surface.NativeHandle
                },
            _ => throw new ArgumentException(
                "Invalid output-surface kind or native handle combination.",
                nameof(surface))
        };

    private static NativeMediaLocatorKind ToNativeLocatorKind(MediaLocatorKind kind) =>
        kind switch
        {
            MediaLocatorKind.File => NativeMediaLocatorKind.File,
            MediaLocatorKind.Directory => NativeMediaLocatorKind.Directory,
            MediaLocatorKind.Uri => NativeMediaLocatorKind.Uri,
            MediaLocatorKind.OpticalDisc => NativeMediaLocatorKind.OpticalDisc,
            _ => throw new ArgumentOutOfRangeException(nameof(kind), kind, null)
        };

    private static void ThrowForResult(NativeResult result, string operation)
    {
        switch (result)
        {
            case NativeResult.Ok:
                return;
            case NativeResult.InvalidArgument:
                throw new ArgumentException(
                    $"Native media runtime rejected arguments for '{operation}'.");
            case NativeResult.InvalidState:
                throw new InvalidOperationException(
                    $"Native media runtime rejected '{operation}' in the current state.");
            case NativeResult.StaleEpoch:
                throw new InvalidOperationException(
                    $"Native media runtime rejected stale playback work for '{operation}'.");
            case NativeResult.OutOfMemory:
                throw new OutOfMemoryException(
                    $"Native media runtime ran out of memory during '{operation}'.");
            case NativeResult.AbiMismatch:
                throw new NotSupportedException(
                    $"Native media runtime reported an ABI mismatch during '{operation}'.");
            case NativeResult.Unsupported:
                throw new NotSupportedException(
                    $"Native media runtime does not support '{operation}'.");
            case NativeResult.OutputUnavailable:
                throw new InvalidOperationException(
                    $"Native video output is unavailable during '{operation}'.");
            case NativeResult.Internal:
            default:
                throw new NativeMediaRuntimeException(operation, result);
        }
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    private sealed class RuntimeLease : IDisposable
    {
        private readonly INativeMediaRuntimeApi _api;
        private nint _handle;

        internal RuntimeLease(INativeMediaRuntimeApi api, nint handle)
        {
            _api = api;
            _handle = handle;
        }

        internal nint Handle => _handle != 0
            ? _handle
            : throw new ObjectDisposedException(nameof(RuntimeLease));

        public void Dispose()
        {
            var handle = _handle;
            if (handle == 0)
            {
                return;
            }

            _handle = 0;
            _api.RuntimeDestroy(handle);
        }
    }

    private sealed class SessionLease : IDisposable
    {
        private readonly INativeMediaRuntimeApi _api;
        private nint _handle;

        internal SessionLease(INativeMediaRuntimeApi api, nint handle)
        {
            _api = api;
            _handle = handle;
        }

        internal nint Handle => _handle != 0
            ? _handle
            : throw new ObjectDisposedException(nameof(SessionLease));

        public void Dispose()
        {
            var handle = _handle;
            if (handle == 0)
            {
                return;
            }

            _handle = 0;
            _api.SessionDestroy(handle);
        }
    }
}
