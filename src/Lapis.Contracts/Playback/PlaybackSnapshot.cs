using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Errors;
using Lapis.Contracts.Time;

namespace Lapis.Contracts.Playback;

public sealed record PlaybackSnapshot(
    PlaybackSessionId SessionId,
    PlaybackEpoch Epoch,
    MediaTime Position,
    TransportState Transport,
    NavigationState Navigation,
    OutputState Output,
    AiState Ai,
    MediaCapability Capabilities,
    PlayerError? LastError,
    MediaTime? Duration = null)
{
    public static PlaybackSnapshot CreateInitial(PlaybackSessionId sessionId) =>
        new(
            sessionId,
            PlaybackEpoch.Initial,
            MediaTime.Zero,
            TransportState.Closed,
            NavigationState.None,
            OutputState.Ready,
            AiState.Off,
            MediaCapability.None,
            null);
}
