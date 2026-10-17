using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;
using Xunit;

namespace Lapis.Contracts.Tests;

public sealed class MediaTimeTests
{
    [Fact]
    public void TimeSpanRoundTripPreservesTickPrecision()
    {
        var input = TimeSpan.FromTicks(12_345_678);

        var mediaTime = MediaTime.FromTimeSpan(input);

        Assert.Equal(input, mediaTime.ToTimeSpan());
        Assert.Equal(input.Ticks * 100L, mediaTime.Nanoseconds);
    }

    [Fact]
    public void MediaTimeCanRepresentNegativePreroll()
    {
        var mediaTime = MediaTime.FromMilliseconds(-250);

        Assert.Equal(-250_000_000L, mediaTime.Nanoseconds);
    }

    [Fact]
    public void EpochIsMonotonic()
    {
        var next = PlaybackEpoch.Initial.Next();

        Assert.Equal(1UL, next.Value);
        Assert.Equal(2UL, next.Next().Value);
    }
}
