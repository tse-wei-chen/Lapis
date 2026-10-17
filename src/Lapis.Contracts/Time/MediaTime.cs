namespace Lapis.Contracts.Time;

/// <summary>
/// Canonical time representation inside Lapis. Source-specific time bases must be
/// converted at the adapter boundary before entering the core.
/// </summary>
public readonly record struct MediaTime(long Nanoseconds) : IComparable<MediaTime>
{
    public static MediaTime Zero => new(0);

    public static MediaTime FromTimeSpan(TimeSpan value) =>
        new(checked(value.Ticks * 100L));

    public static MediaTime FromMilliseconds(long milliseconds) =>
        new(checked(milliseconds * 1_000_000L));

    public static MediaTime FromSeconds(long seconds) =>
        new(checked(seconds * 1_000_000_000L));

    public TimeSpan ToTimeSpan() => TimeSpan.FromTicks(Nanoseconds / 100L);

    public int CompareTo(MediaTime other) => Nanoseconds.CompareTo(other.Nanoseconds);

    public static MediaTime operator +(MediaTime left, MediaTime right) =>
        new(checked(left.Nanoseconds + right.Nanoseconds));

    public static MediaTime operator -(MediaTime left, MediaTime right) =>
        new(checked(left.Nanoseconds - right.Nanoseconds));
}
