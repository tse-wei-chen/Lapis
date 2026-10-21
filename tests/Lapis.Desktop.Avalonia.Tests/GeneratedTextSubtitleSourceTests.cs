using Xunit;
using System.Globalization;
using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.Services;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class GeneratedTextSubtitleSourceTests
{
    [Fact]
    public async Task ExplicitSelectionUsesSharedSnapshotButNeverStartsAI()
    {
        var active = true;
        var timeline = new GeneratedSubtitleTimeline();
        var generation = timeline.BeginGeneration();
        var source = new GeneratedTextSubtitleSource(
            "movie.mkv", "AI translation (manual)", "zh-TW",
            timeline, () => active);
        var descriptor = Assert.Single(await source.DiscoverAsync(
            "movie.mkv", CancellationToken.None));
        Assert.Equal(TextSubtitleSourceKind.GeneratedText, descriptor.Kind);
        Assert.Equal("zh-TW", descriptor.LanguageTag);

        var selected = await source.LoadAsync(
            "movie.mkv", descriptor, CancellationToken.None);
        Assert.Same(timeline, selected);
        Assert.True(timeline.TryPublish(generation, 0,
            [new(TimeSpan.Zero, TimeSpan.FromSeconds(2), "翻譯")]));
        Assert.Equal("翻譯", selected.At(TimeSpan.FromSeconds(1)));

        var auto = await source.LoadPreferredAsync(
            "movie.mkv", CultureInfo.InvariantCulture, CancellationToken.None);
        Assert.Null(auto.At(TimeSpan.FromSeconds(1)));
        active = false;
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            source.DiscoverAsync("movie.mkv", CancellationToken.None));
    }

    [Fact]
    public async Task RejectsWrongMediaForgedDescriptorAndCancellation()
    {
        var source = new GeneratedTextSubtitleSource(
            "a.mkv", "Generated", null,
            new GeneratedSubtitleTimeline(), () => true);
        var descriptor = Assert.Single(await source.DiscoverAsync(
            "a.mkv", CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(() =>
            source.LoadAsync("b.mkv", descriptor, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(() =>
            source.LoadAsync("a.mkv", descriptor with { TrackId = "forged" },
                CancellationToken.None));
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            source.DiscoverAsync("a.mkv", cancelled.Token));
    }
}
