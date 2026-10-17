namespace Lapis.Contracts.Playback;

public readonly record struct PlaybackSessionId(Guid Value)
{
    public static PlaybackSessionId New() => new(Guid.NewGuid());

    public override string ToString() => Value.ToString("D");
}
