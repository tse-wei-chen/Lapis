using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Optional capability of the active player service. All calls are bound to
/// one captured playback snapshot and are serialized with Open/Seek/Close by
/// DesktopPlaybackHost. Test doubles can implement this without loading native
/// DLLs; views do not depend on FFmpeg or P/Invoke.
/// </summary>
public interface IEmbeddedSubtitlePlaybackService
{
    PlaybackSnapshot Snapshot { get; }

    Task<bool> SupportsEmbeddedTextSubtitleCuesAsync(
        PlaybackSnapshot expected, CancellationToken cancellationToken);

    Task<IReadOnlyList<EmbeddedSubtitleTrack>> ReadEmbeddedSubtitleTracksAsync(
        PlaybackSnapshot expected, CancellationToken cancellationToken);

    Task StartEmbeddedTextSubtitleLoadAsync(
        uint streamIndex, PlaybackSnapshot expected,
        CancellationToken cancellationToken);

    Task<EmbeddedTextSubtitleCuePage> ReadEmbeddedTextSubtitleCuesAsync(
        uint startIndex, PlaybackSnapshot expected, CancellationToken cancellationToken);
}
