namespace Lapis.Contracts.Playback;

/// <summary>
/// Monotonic generation attached to asynchronous playback work. Results from an
/// older epoch are stale and must be discarded after discontinuities such as seek.
/// </summary>
public readonly record struct PlaybackEpoch(ulong Value)
{
    public static PlaybackEpoch Initial => new(0);

    public PlaybackEpoch Next() => new(checked(Value + 1));
}
