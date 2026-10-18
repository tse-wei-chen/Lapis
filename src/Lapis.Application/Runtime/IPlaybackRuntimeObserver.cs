using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;

namespace Lapis.Application.Runtime;

/// <summary>
/// Optional runtime observation port. A read is observational and must never
/// advance the playback epoch, drive commands, or mutate the playback engine.
/// The returned session and epoch must be checked again by the playback actor.
/// </summary>
public interface IPlaybackRuntimeObserver
{
    ValueTask<RuntimePlaybackObservation> ReadSnapshotAsync(
        PlaybackSessionId sessionId,
        CancellationToken cancellationToken);
}

public readonly record struct RuntimePlaybackObservation(
    PlaybackSessionId SessionId,
    PlaybackEpoch Epoch,
    MediaTime Position,
    TransportState Transport);
