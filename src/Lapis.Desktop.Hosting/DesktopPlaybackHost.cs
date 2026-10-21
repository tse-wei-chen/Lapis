using Lapis.Application.Playback;
using Lapis.Application.Runtime;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;

namespace Lapis.Desktop.Hosting;

/// <summary>
/// UI-framework-neutral desktop composition coordinator. All host commands,
/// including output-surface changes, are ordered relative to playback commands.
/// The view owns its HWND; this class never destroys a native window.
/// </summary>
public sealed class DesktopPlaybackHost : IAsyncDisposable
{
    private readonly PlaybackSession _session;
    private readonly IVideoOutputSurfaceBinding _surfaces;
    private readonly IEmbeddedSubtitleTrackInventoryRuntime? _embeddedTracks;
    private readonly IEmbeddedTextSubtitleCueRuntime? _embeddedCues;
    private readonly SemaphoreSlim _operations = new(1, 1);
    private readonly object _disposeGate = new();
    private Task? _disposeTask;
    private int _stopping;
    private bool _hasBoundSurface;

    public DesktopPlaybackHost(
        IPlaybackRuntime playbackRuntime,
        IVideoOutputSurfaceBinding surfaces,
        PlaybackSessionId? sessionId = null)
    {
        ArgumentNullException.ThrowIfNull(playbackRuntime);
        ArgumentNullException.ThrowIfNull(surfaces);
        _surfaces = surfaces;
        _embeddedTracks = playbackRuntime as IEmbeddedSubtitleTrackInventoryRuntime;
        _embeddedCues = playbackRuntime as IEmbeddedTextSubtitleCueRuntime;
        _session = new PlaybackSession(playbackRuntime, sessionId);
    }

    public PlaybackSnapshot Snapshot => _session.Snapshot;

    public Exception? LastShutdownWarning { get; private set; }

    /// <summary>
    /// Metadata and cue operations use the same host command gate as Seek,
    /// Open and Close. Every request is scoped to an expected immutable
    /// playback snapshot, so a queued old-epoch read cannot attach to a
    /// different media generation.
    /// </summary>
    public Task<bool> SupportsEmbeddedTextSubtitleCuesAsync(
        PlaybackSnapshot expected, CancellationToken cancellationToken = default) =>
        RunAsync(async () =>
        {
            _ = SubtitleContext(expected);
            var source = _embeddedCues;
            if (source is null) return false;
            // Version resolution runs off the UI dispatcher just like the
            // other native subtitle queries.
            return await Task.Run(() => source.SupportsEmbeddedTextSubtitleCues,
                cancellationToken).ConfigureAwait(false);
        }, cancellationToken);

    public Task<IReadOnlyList<EmbeddedSubtitleTrack>> ReadEmbeddedSubtitleTracksAsync(
        PlaybackSnapshot expected, CancellationToken cancellationToken = default) =>
        RunAsync(async () =>
        {
            var context = SubtitleContext(expected);
            var source = _embeddedTracks ?? throw new NotSupportedException(
                "This runtime does not provide embedded subtitle metadata.");
            return await source.ReadEmbeddedSubtitleTracksAsync(
                context, cancellationToken).ConfigureAwait(false);
        }, cancellationToken);

    public Task StartEmbeddedTextSubtitleLoadAsync(
        uint streamIndex, PlaybackSnapshot expected,
        CancellationToken cancellationToken = default) =>
        RunAsync(async () =>
        {
            var context = SubtitleContext(expected);
            var source = _embeddedCues ?? throw new NotSupportedException(
                "This runtime does not provide embedded text subtitles.");
            await source.StartEmbeddedTextSubtitleLoadAsync(
                streamIndex, context, cancellationToken).ConfigureAwait(false);
        }, cancellationToken);

    public Task<EmbeddedTextSubtitleCuePage> ReadEmbeddedTextSubtitleCuesAsync(
        uint startIndex, PlaybackSnapshot expected,
        CancellationToken cancellationToken = default) =>
        RunAsync(async () =>
        {
            var context = SubtitleContext(expected);
            var source = _embeddedCues ?? throw new NotSupportedException(
                "This runtime does not provide embedded text subtitles.");
            return await source.ReadEmbeddedTextSubtitleCuesAsync(
                startIndex, context, cancellationToken).ConfigureAwait(false);
        }, cancellationToken);

    private RuntimeCommandContext SubtitleContext(PlaybackSnapshot expected)
    {
        ArgumentNullException.ThrowIfNull(expected);
        var active = _session.Snapshot;
        if (active.SessionId != expected.SessionId ||
            active.Epoch != expected.Epoch ||
            active.Transport is not (TransportState.Ready or TransportState.Playing
                or TransportState.Paused or TransportState.Ended))
            throw new InvalidOperationException(
                "Subtitle request belongs to a retired or closed playback generation.");
        return new RuntimeCommandContext(active.SessionId, active.Epoch);
    }


    public IAsyncEnumerable<PlaybackSnapshot> ObserveSnapshotsAsync(
        CancellationToken cancellationToken = default) =>
        _session.ObserveSnapshotsAsync(cancellationToken);

    /// <summary>
    /// Backward-compatible file opening shortcut. Other sources use the typed
    /// locator overload so they pass through the same application actor.
    /// </summary>
    public Task OpenAndPlayAsync(
        string path,
        nint windowHandle,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("A media file is required.", nameof(path));
        }
        return OpenAndPlayAsync(
            new MediaLocator(MediaLocatorKind.File, path),
            windowHandle,
            cancellationToken);
    }

    /// <summary>
    /// Open any supported media locator, bind the HWND at its new playback
    /// epoch, then play. Source type never changes the host's ordering or
    /// native window ownership rules.
    /// </summary>
    public Task OpenAndPlayAsync(
        MediaLocator locator,
        nint windowHandle,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(locator);
        if (windowHandle == 0)
        {
            throw new ArgumentException("The video window must have a valid HWND.",
                nameof(windowHandle));
        }

        return RunAsync(async () =>
        {
            await _session.ExecuteAsync(
                new OpenMediaCommand(locator),
                cancellationToken).ConfigureAwait(false);
            await SetSurfaceCoreAsync(
                VideoOutputSurface.ForWin32Window(windowHandle),
                cancellationToken).ConfigureAwait(false);
            await _session.ExecuteAsync(
                new PlayCommand(),
                cancellationToken).ConfigureAwait(false);
        }, cancellationToken);
    }

    public Task TogglePlayPauseAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(async () =>
        {
            var command = _session.Snapshot.Transport switch
            {
                TransportState.Playing => (PlaybackCommand)new PauseCommand(),
                TransportState.Ready or TransportState.Paused => new PlayCommand(),
                _ => throw new InvalidOperationException(
                    "Playback must be ready, playing or paused.")
            };
            await _session.ExecuteAsync(command, cancellationToken).ConfigureAwait(false);
        }, cancellationToken);

    public Task SeekAsync(
        TimeSpan position,
        CancellationToken cancellationToken = default)
    {
        if (position < TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(position));
        }

        var mediaPosition = MediaTime.FromTimeSpan(position);
        return RunAsync(async () =>
        {
            await _session.ExecuteAsync(
                new SeekCommand(mediaPosition),
                cancellationToken).ConfigureAwait(false);
        }, cancellationToken);
    }

    /// <summary>
    /// Allows a desktop framework to replace an HWND after the new handle is
    /// created. Successful completion releases the previous HWND before return.
    /// </summary>
    public Task<VideoOutputSurfaceBindingState> ReplaceWindowAsync(
        nint windowHandle,
        CancellationToken cancellationToken = default)
    {
        if (windowHandle == 0)
        {
            throw new ArgumentException("The replacement HWND cannot be zero.",
                nameof(windowHandle));
        }

        return RunAsync(() => SetSurfaceCoreAsync(
            VideoOutputSurface.ForWin32Window(windowHandle),
            cancellationToken), cancellationToken);
    }

    /// <summary>
    /// Must complete before the UI destroys or recreates a borrowed HWND.
    /// Serialized with Open/Play/Seek/Close; a successful native ACK guarantees
    /// the presenter has released the previous window.
    /// </summary>
    public Task<VideoOutputSurfaceBindingState> DetachWindowAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(async () =>
        {
            if (!_hasBoundSurface)
            {
                return VideoOutputSurfaceBindingState.Deferred;
            }

            return await SetSurfaceCoreAsync(
                VideoOutputSurface.None,
                cancellationToken).ConfigureAwait(false);
        }, cancellationToken);

    public Task CloseMediaAsync(
        CancellationToken cancellationToken = default) =>
        RunAsync(async () =>
        {
            await _session.ExecuteAsync(
                new CloseCommand(),
                cancellationToken).ConfigureAwait(false);
            if (_hasBoundSurface)
            {
                await SetSurfaceCoreAsync(
                    VideoOutputSurface.None,
                    cancellationToken).ConfigureAwait(false);
            }
        }, cancellationToken);

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
        {
            _disposeTask ??= DisposeCoreAsync();
            return new ValueTask(_disposeTask);
        }
    }

    private async Task DisposeCoreAsync()
    {
        // Existing host commands finish before shutdown releases the HWND.
        // New commands fail at the gate even while the old command completes.
        Volatile.Write(ref _stopping, 1);
        await _operations.WaitAsync().ConfigureAwait(false);
        Exception? detachFailure = null;
        try
        {
            if (_hasBoundSurface)
            {
                try
                {
                    await SetSurfaceCoreAsync(
                        VideoOutputSurface.None,
                        CancellationToken.None).ConfigureAwait(false);
                }
                catch (Exception exception)
                {
                    // Disposal below remains the final safety barrier: it joins
                    // native render threads even if a live detach was rejected.
                    detachFailure = exception;
                }
            }

            await _session.DisposeAsync().ConfigureAwait(false);
        }
        finally
        {
            _operations.Release();
        }

        // A native session disposal joins the render worker and releases all
        // borrowed HWND resources. A failed live detach is therefore not fatal
        // after a successful complete disposal; expose it for diagnostics.
        LastShutdownWarning = detachFailure;
    }

    private async Task<VideoOutputSurfaceBindingState> SetSurfaceCoreAsync(
        VideoOutputSurface surface,
        CancellationToken cancellationToken)
    {
        var context = new RuntimeCommandContext(_session.SessionId, _session.Snapshot.Epoch);
        // The existing C ABI waits synchronously for render-thread ACK. Never
        // execute that wait on any desktop UI dispatcher thread.
        var result = await Task.Run(async () =>
            await _surfaces.SetVideoOutputSurfaceAsync(
                surface, context, cancellationToken).ConfigureAwait(false),
            cancellationToken).ConfigureAwait(false);

        _hasBoundSurface = surface.Kind != VideoOutputSurfaceKind.None;
        return result;
    }

    private async Task RunAsync(
        Func<Task> operation,
        CancellationToken cancellationToken)
    {
        await _operations.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(
                Volatile.Read(ref _stopping) != 0, this);
            await operation().ConfigureAwait(false);
        }
        finally
        {
            _operations.Release();
        }
    }

    private async Task<T> RunAsync<T>(
        Func<Task<T>> operation,
        CancellationToken cancellationToken)
    {
        await _operations.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(
                Volatile.Read(ref _stopping) != 0, this);
            return await operation().ConfigureAwait(false);
        }
        finally
        {
            _operations.Release();
        }
    }
}
