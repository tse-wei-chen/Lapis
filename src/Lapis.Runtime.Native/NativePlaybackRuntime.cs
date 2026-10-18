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
public sealed class NativePlaybackRuntime : IPlaybackRuntime, IVideoOutputSurfaceBinding, IPlaybackRuntimeObserver
{
    private readonly object _gate = new();
    private readonly INativeMediaRuntimeApi _api;
    private readonly RuntimeLease _runtime;
    private SessionLease? _session;
    private PlaybackSessionId? _boundSessionId;
    private bool _disposed;

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
            var session = EnsureSession(context.SessionId);
            var kind = ToNativeLocatorKind(locator.Kind);
            var epoch = new NativePlaybackEpoch { Value = context.Epoch.Value };
            var result = _api.SessionOpen(
                session,
                kind,
                locator.Value,
                epoch,
                out var openResult);
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
