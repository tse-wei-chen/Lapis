using Lapis.Contracts.Media;

namespace Lapis.Application.Runtime;

/// <summary>
/// Optional observational port for embedded subtitle stream metadata.
/// Must not create playback sessions, decode packets or change playback Epoch.
/// Call it only after an Open operation has established the session.
/// </summary>
public interface IEmbeddedSubtitleTrackInventoryRuntime
{
    ValueTask<IReadOnlyList<EmbeddedSubtitleTrack>> ReadEmbeddedSubtitleTracksAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken);
}
