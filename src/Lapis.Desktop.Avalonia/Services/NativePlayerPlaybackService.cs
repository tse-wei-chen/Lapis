using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Hosting;
using Lapis.Runtime.Native;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// The one composition root for the Windows Native runtime. Instantiate lazily
/// so the Avalonia UI can start and report a missing DLL instead of crashing.
/// The existing UI-neutral DesktopPlaybackHost owns every playback command.
/// </summary>
public sealed class NativePlayerPlaybackService(Func<nint> getVideoHwnd)
    : IPlayerPlaybackService
{
    private readonly PlaybackSnapshot _initial =
        PlaybackSnapshot.CreateInitial(PlaybackSessionId.New());
    private readonly Func<nint> _getVideoHwnd =
        getVideoHwnd ?? throw new ArgumentNullException(nameof(getVideoHwnd));
    private DesktopPlaybackHost? _host;
    private bool _disposed;

    public PlaybackSnapshot Snapshot => _host?.Snapshot ?? _initial;

    public async Task OpenAsync(MediaLocator locator, CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        var output = _getVideoHwnd(); // UI thread creates/owns the child HWND.
        if (output == 0)
            throw new InvalidOperationException("Video output window is not ready.");

        if (_host is null)
        {
            var runtime = new NativePlaybackRuntime();
            _host = new DesktopPlaybackHost(runtime, runtime);
        }

        await _host.OpenAndPlayAsync(locator, output, cancellationToken);
    }

    public Task ToggleAsync(CancellationToken cancellationToken) =>
        RequireHost().TogglePlayPauseAsync(cancellationToken);

    public Task SeekAsync(TimeSpan position, CancellationToken cancellationToken) =>
        RequireHost().SeekAsync(position, cancellationToken);

    public Task CloseAsync(CancellationToken cancellationToken) =>
        _host?.CloseMediaAsync(cancellationToken) ?? Task.CompletedTask;

    public IAsyncEnumerable<PlaybackSnapshot> ObserveSnapshotsAsync(
        CancellationToken cancellationToken) =>
        RequireHost().ObserveSnapshotsAsync(cancellationToken);

    private DesktopPlaybackHost RequireHost() =>
        _host ?? throw new InvalidOperationException("No media has been opened.");

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        if (_host is { } host)
        {
            await host.DisposeAsync();
            _host = null;
        }
    }
}
