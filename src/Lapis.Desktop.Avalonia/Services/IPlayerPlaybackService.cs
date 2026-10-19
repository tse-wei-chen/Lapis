using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// UI-facing port: ViewModels never see HWND, D3D11, libbluray or the C ABI.
/// Another OS can supply another surface adapter without changing the views.
/// </summary>
public interface IPlayerPlaybackService : IAsyncDisposable
{
    PlaybackSnapshot Snapshot { get; }
    Task OpenAsync(MediaLocator locator, CancellationToken cancellationToken);
    Task ToggleAsync(CancellationToken cancellationToken);
    Task SeekAsync(TimeSpan position, CancellationToken cancellationToken);
    Task CloseAsync(CancellationToken cancellationToken);
    IAsyncEnumerable<PlaybackSnapshot> ObserveSnapshotsAsync(
        CancellationToken cancellationToken);
}
