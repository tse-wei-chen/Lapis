using System.Globalization;
using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class TextSubtitleSourceCatalogTests
{
    private const string Movie = "movie.mkv";

    private sealed class FakeSource(string id, string text) : ITextSubtitleSource
    {
        public string SourceId => id;
        public int DiscoveryCalls { get; private set; }
        public int LoadCalls { get; private set; }
        public int AutoCalls { get; private set; }
        public bool ReturnWrongOwner { get; set; }
        public string TrackId { get; set; } = "track-1";

        public Task<IReadOnlyList<TextSubtitleDescriptor>> DiscoverAsync(
            string mediaPath, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ++DiscoveryCalls;
            IReadOnlyList<TextSubtitleDescriptor> found =
            [
                new(ReturnWrongOwner ? "unknown" : id, TrackId, text, "ja-JP",
                    TextSubtitleSourceKind.GeneratedText)
            ];
            return Task.FromResult(found);
        }

        public Task<ITextSubtitleTrack> LoadAsync(
            string mediaPath, TextSubtitleDescriptor descriptor,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(SourceId, descriptor.SourceId);
            Assert.Equal(TrackId, descriptor.TrackId);
            ++LoadCalls;
            ITextSubtitleTrack track = SidecarSubtitleTrack.Parse(
                $"1\n00:00:00,000 --> 00:00:05,000\n{text}\n");
            return Task.FromResult(track);
        }

        public Task<ITextSubtitleTrack> LoadPreferredAsync(
            string mediaPath, CultureInfo culture, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ++AutoCalls;
            ITextSubtitleTrack track = SidecarSubtitleTrack.Parse(
                $"1\n00:00:00,000 --> 00:00:05,000\n{text} auto\n");
            return Task.FromResult(track);
        }
    }

    [Fact]
    public async Task SameOpaqueTrackIdAcrossSourcesStaysDistinctAndRoutesCorrectly()
    {
        var local = new FakeSource("local", "Local");
        var generated = new FakeSource("generated-ai", "Translated");
        var catalog = new TextSubtitleSourceCatalog([local, generated]);

        var tracks = await catalog.DiscoverAsync(Movie);
        Assert.Equal(2, tracks.Count);
        Assert.Equal("track-1", tracks[0].TrackId);
        Assert.Equal("track-1", tracks[1].TrackId);
        Assert.NotEqual(tracks[0].SourceId, tracks[1].SourceId);
        Assert.Equal("Local", (await catalog.LoadAsync(Movie, tracks[0]))
            .At(TimeSpan.FromSeconds(1)));
        Assert.Equal("Translated", (await catalog.LoadAsync(Movie, tracks[1]))
            .At(TimeSpan.FromSeconds(1)));
        Assert.Equal(1, local.LoadCalls);
        Assert.Equal(1, generated.LoadCalls);
    }

    [Fact]
    public async Task AutoUsesFirstSourceAndDoesNotImplicitlyStartGeneratedTranslation()
    {
        var sidecar = new FakeSource("sidecar", "Existing SRT");
        var generated = new FakeSource("ai", "Generated");
        var catalog = new TextSubtitleSourceCatalog([sidecar, generated]);

        var active = await catalog.LoadPreferredAsync(
            Movie, CultureInfo.GetCultureInfo("zh-TW"));
        Assert.Equal("Existing SRT auto", active.At(TimeSpan.FromSeconds(2)));
        Assert.Equal(1, sidecar.AutoCalls);
        Assert.Equal(0, generated.AutoCalls);
        Assert.Equal(0, generated.LoadCalls);
    }

    [Fact]
    public async Task RegistryRejectsConflictingSourceIdsAndForgedOwner()
    {
        Assert.Throws<ArgumentException>(() => new TextSubtitleSourceCatalog(
            [new FakeSource("same", "one"), new FakeSource("same", "two")]));

        var source = new FakeSource("valid", "cue") { ReturnWrongOwner = true };
        var catalog = new TextSubtitleSourceCatalog([source]);
        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await catalog.DiscoverAsync(Movie));

        await Assert.ThrowsAsync<ArgumentException>(
            async () => await catalog.LoadAsync(Movie,
                new TextSubtitleDescriptor("unknown", "track-1", "bad", null,
                    TextSubtitleSourceKind.EmbeddedText)));
    }

    [Fact]
    public async Task CancellationPreventsAnyProviderDiscoveryOrLoad()
    {
        var source = new FakeSource("local", "cue");
        var catalog = new TextSubtitleSourceCatalog([source]);
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await catalog.DiscoverAsync(Movie, cancelled.Token));
        Assert.Equal(0, source.DiscoveryCalls);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await catalog.LoadAsync(Movie, new TextSubtitleDescriptor(
                source.SourceId, "track-1", "cue", null,
                TextSubtitleSourceKind.LocalSidecar), cancelled.Token));
        Assert.Equal(0, source.LoadCalls);
    }

    [Fact]
    public async Task SrtProviderCanBeDiscoveredAndLoadedThroughRegistry()
    {
        var directory = Path.Combine(Path.GetTempPath(),
            "lapis-text-sources-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var movie = Path.Combine(directory, "clip.mkv");
            var subtitle = Path.Combine(directory, "clip.zh-TW.srt");
            await File.WriteAllTextAsync(subtitle,
                "1\n00:00:01,000 --> 00:00:04,000\n字幕來源\n");

            var catalog = TextSubtitleSourceCatalog.Default;
            var options = await catalog.DiscoverAsync(movie);
            var selected = Assert.Single(options);
            Assert.Equal(SidecarSrtTextSubtitleSource.Id, selected.SourceId);
            Assert.Equal("zh-TW", selected.LanguageTag);
            Assert.Equal(TextSubtitleSourceKind.LocalSidecar, selected.Kind);
            Assert.Equal("字幕來源", (await catalog.LoadAsync(movie, selected))
                .At(TimeSpan.FromSeconds(2)));
            Assert.Equal("字幕來源", (await catalog.LoadPreferredAsync(movie,
                CultureInfo.GetCultureInfo("zh-TW"))).At(TimeSpan.FromSeconds(2)));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task SrtProviderRefusesDescriptorsOutsideCurrentMovie()
    {
        var catalog = TextSubtitleSourceCatalog.Default;
        var directory = Path.Combine(Path.GetTempPath(),
            "lapis-subtitle-guard-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var movie = Path.Combine(directory, "title.mkv");
            var outside = Path.Combine(Path.GetTempPath(), "another.srt");
            var forged = new TextSubtitleDescriptor(SidecarSrtTextSubtitleSource.Id,
                outside, "another.srt", null, TextSubtitleSourceKind.LocalSidecar);
            await Assert.ThrowsAsync<ArgumentException>(
                async () => await catalog.LoadAsync(movie, forged));

            var wrongName = new TextSubtitleDescriptor(SidecarSrtTextSubtitleSource.Id,
                Path.Combine(directory, "unrelated.zh-TW.srt"), "unrelated.zh-TW.srt",
                "zh-TW", TextSubtitleSourceKind.LocalSidecar);
            await Assert.ThrowsAsync<ArgumentException>(
                async () => await catalog.LoadAsync(movie, wrongName));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }
}
