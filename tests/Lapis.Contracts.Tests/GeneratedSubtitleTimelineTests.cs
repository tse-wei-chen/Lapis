using Xunit;
using Lapis.Contracts.Media;

namespace Lapis.Contracts.Tests;

public sealed class GeneratedSubtitleTimelineTests
{
    private static GeneratedSubtitleCue Cue(double start, double end, string text) =>
        new(TimeSpan.FromSeconds(start), TimeSpan.FromSeconds(end), text);

    [Fact]
    public void SnapshotPublishesAtomicallyAndUsesMediaTime()
    {
        var timeline = new GeneratedSubtitleTimeline();
        var epoch = timeline.BeginGeneration();
        Assert.True(timeline.TryPublish(epoch, 0,
            [Cue(4, 6, "late"), Cue(1, 3, "early")]));
        Assert.Null(timeline.At(TimeSpan.FromMilliseconds(999)));
        Assert.Equal("early", timeline.At(TimeSpan.FromSeconds(1)));
        Assert.Null(timeline.At(TimeSpan.FromSeconds(3)));
        Assert.Equal("late", timeline.At(TimeSpan.FromSeconds(5)));
        Assert.Null(timeline.At(TimeSpan.FromSeconds(6)));
        Assert.Null(timeline.At(TimeSpan.FromSeconds(-1)));
    }

    [Fact]
    public void SeekRejectsLateGenerationsAndStaleRevisions()
    {
        var timeline = new GeneratedSubtitleTimeline();
        var before = timeline.BeginGeneration();
        Assert.True(timeline.TryPublish(before, 3, [Cue(0, 10, "old")]));
        var after = timeline.BeginGeneration();
        Assert.Null(timeline.At(TimeSpan.FromSeconds(1)));
        Assert.False(timeline.TryPublish(before, 999, [Cue(0, 10, "late")]));
        Assert.True(timeline.TryPublish(after, 0, [Cue(0, 10, "new")]));
        Assert.False(timeline.TryPublish(after, 0, [Cue(0, 10, "duplicate")]));
        Assert.False(timeline.TryPublish(after, 0, []));
        Assert.Equal("new", timeline.At(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public void OverlappingCuesUseLatestStartAndFallBackWhenItEnds()
    {
        var timeline = new GeneratedSubtitleTimeline();
        var generation = timeline.BeginGeneration();
        Assert.True(timeline.TryPublish(generation, 0,
            [Cue(0, 10, "background"), Cue(2, 3, "foreground")]));
        Assert.Equal("foreground", timeline.At(TimeSpan.FromSeconds(2.5)));
        Assert.Equal("background", timeline.At(TimeSpan.FromSeconds(4)));
    }

    [Fact]
    public void InvalidOrOversizedBatchesDoNotDestroyPreviousSnapshot()
    {
        var timeline = new GeneratedSubtitleTimeline();
        var generation = timeline.BeginGeneration();
        Assert.True(timeline.TryPublish(generation, 0, [Cue(0, 2, "ok")]));
        Assert.Throws<ArgumentException>(() => timeline.TryPublish(generation, 1,
            [Cue(3, 3, "zero duration")]));
        Assert.Throws<ArgumentException>(() => timeline.TryPublish(generation, 1,
            [Cue(0, 2, new string('a', GeneratedSubtitleTimeline.MaxTextLength + 1))]));
        Assert.Throws<ArgumentException>(() => timeline.TryPublish(generation, 1,
            Enumerable.Repeat(Cue(0, 1, "many"),
                GeneratedSubtitleTimeline.MaxCues + 1).ToArray()));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            timeline.TryPublish(generation, -1, []));
        Assert.Equal("ok", timeline.At(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task ConcurrentPublishAndReadsNeverObservePartiallyWrittenCues()
    {
        var timeline = new GeneratedSubtitleTimeline();
        var generation = timeline.BeginGeneration();
        var writers = Task.Run(() =>
        {
            for (var i = 0; i < 1000; i++)
                Assert.True(timeline.TryPublish(generation, i,
                    [Cue(0, 5, i.ToString())]));
        });
        var readers = Task.Run(() =>
        {
            for (var i = 0; i < 1000; i++)
            {
                var text = timeline.At(TimeSpan.FromSeconds(1));
                if (text is not null) Assert.True(int.TryParse(text, out _));
            }
        });
        await Task.WhenAll(writers, readers);
        Assert.Equal("999", timeline.At(TimeSpan.FromSeconds(1)));
    }
}
