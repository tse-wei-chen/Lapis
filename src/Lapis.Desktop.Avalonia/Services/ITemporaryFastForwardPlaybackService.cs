using Lapis.Contracts.Playback;

namespace Lapis.Desktop.Avalonia.Services;

public interface ITemporaryFastForwardPlaybackService
{
    Task<bool> SupportsTemporaryFastForwardAsync(
        PlaybackSnapshot expected, CancellationToken cancellationToken);

    Task SetTemporaryFastForwardAsync(
        bool enabled, PlaybackSnapshot expected,
        CancellationToken cancellationToken);
}
