using System.Globalization;
using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class WebVttSubtitleSourceTests
{
    [Fact]
    public void ParsesIdentifiersCueSettingsMarkupAndTimeBoundaries()
    {
        var track = SidecarWebVttTextSubtitleSource.Parse(
            "\uFEFFWEBVTT - exported subtitles\r\n\r\n" +
            "NOTE translator comment\r\n00:00:00.000 --> 00:01:00.000\r\nnot visible\r\n\r\n" +
            "STYLE\r\n::cue { color: red }\r\n\r\n" +
            "REGION\r\nid:top\r\n\r\n" +
            "cue-id\r\n" +
            "00:01.000 --> 00:04.000 align:start position:10%\r\n" +
            "<v Narrator><i>Hello &amp; 世界</i></v>\r\n\r\n" +
            "00:02.000 --> 00:03.000\r\nShort\r\n\r\n" +
            "01:02:03.100 --> 01:02:04.200\r\nHours work\r\n");

        Assert.Equal(3, track.Count);
        Assert.Null(track.At(TimeSpan.FromMilliseconds(999)));
        Assert.Equal("Hello & 世界", track.At(TimeSpan.FromSeconds(1)));
        Assert.Equal("Short", track.At(TimeSpan.FromSeconds(2.5)));
        Assert.Equal("Hello & 世界", track.At(TimeSpan.FromSeconds(3)));
        Assert.Null(track.At(TimeSpan.FromSeconds(4)));
        Assert.Equal("Hours work", track.At(new TimeSpan(1, 2, 3).Add(
            TimeSpan.FromMilliseconds(150))));
        Assert.Null(track.At(new TimeSpan(1, 2, 4).Add(
            TimeSpan.FromMilliseconds(200))));
    }

    [Fact]
    public void MalformedOrOversizedTextProducesNoActiveCues()
    {
        Assert.Equal(0, SidecarWebVttTextSubtitleSource.Parse(
            "00:00.000 --> 00:02.000\nThis is not WebVTT\n").Count);
        Assert.Equal(0, SidecarWebVttTextSubtitleSource.Parse(
            "WEBVTT\n\n00:01.000 --> 00:00.900\nWrong order\n").Count);
        Assert.Equal(0, SidecarWebVttTextSubtitleSource.Parse(
            "WEBVTT\n\n00:61.000 --> 00:62.000\nInvalid minutes\n").Count);
        Assert.Equal(0, SidecarWebVttTextSubtitleSource.Parse(
            "WEBVTT\n\n9999999999999999999:00:00.000 --> 9999999999999999999:01:00.000\nOverflow\n").Count);
        Assert.Equal(0, SidecarWebVttTextSubtitleSource.Parse(
            new string('x', 2_000_001)).Count);
    }

    [Fact]
    public async Task WebVttCanBeSelectedThroughCatalogWhileSrtRemainsAuto()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-vtt-provider-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var media = Path.Combine(folder, "show.mkv");
            await File.WriteAllTextAsync(Path.Combine(folder, "show.srt"),
                "1\n00:00:00,000 --> 00:00:10,000\nSRT auto\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "show.ja-JP.vtt"),
                "WEBVTT\n\n00:00.000 --> 00:10.000\nJapanese VTT\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "show.vtt"),
                "WEBVTT\n\n00:00.000 --> 00:10.000\nDefault VTT\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "show.vtt.backup"),
                "WEBVTT\n\n00:00.000 --> 00:10.000\nIgnore\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "show.zh_TW.vtt"),
                "WEBVTT\n\n00:00.000 --> 00:10.000\nIgnore\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "another.ja-JP.vtt"),
                "WEBVTT\n\n00:00.000 --> 00:10.000\nIgnore\n");

            var catalog = TextSubtitleSourceCatalog.Default;
            var tracks = await catalog.DiscoverAsync(media);
            Assert.Equal(3, tracks.Count);
            var japanese = Assert.Single(tracks,
                track => track.DisplayName == "show.ja-JP.vtt");
            Assert.Equal(SidecarWebVttTextSubtitleSource.Id, japanese.SourceId);
            Assert.Equal("ja-JP", japanese.LanguageTag);
            Assert.Equal(TextSubtitleSourceKind.LocalSidecar, japanese.Kind);
            Assert.Equal("Japanese VTT", (await catalog.LoadAsync(media, japanese))
                .At(TimeSpan.FromSeconds(2)));
            Assert.Equal("SRT auto", (await catalog.LoadPreferredAsync(media,
                CultureInfo.GetCultureInfo("ja-JP"))).At(TimeSpan.FromSeconds(2)));
        }
        finally { Directory.Delete(folder, recursive: true); }
    }

    [Fact]
    public async Task CannotLoadUnrelatedOrDifferentProviderDescriptors()
    {
        var provider = new SidecarWebVttTextSubtitleSource();
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-vtt-guard-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var media = Path.Combine(folder, "movie.mp4");
            var outside = Path.Combine(Path.GetTempPath(), "wrong.vtt");
            var wrong = new TextSubtitleDescriptor(
                provider.SourceId, outside, "wrong.vtt", null,
                TextSubtitleSourceKind.LocalSidecar);
            await Assert.ThrowsAsync<ArgumentException>(
                async () => await provider.LoadAsync(media, wrong, CancellationToken.None));

            var forged = wrong with
            {
                TrackId = Path.Combine(folder, "another.zh-TW.vtt")
            };
            await Assert.ThrowsAsync<ArgumentException>(
                async () => await provider.LoadAsync(media, forged, CancellationToken.None));

            var foreign = wrong with { SourceId = SidecarSrtTextSubtitleSource.Id };
            await Assert.ThrowsAsync<ArgumentException>(
                async () => await provider.LoadAsync(media, foreign, CancellationToken.None));
        }
        finally { Directory.Delete(folder, recursive: true); }
    }

    [Fact]
    public async Task DiscoveryAndReadHonorCancellationAndFileLimit()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-vtt-cancel-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var media = Path.Combine(folder, "feature.mkv");
            var subtitle = Path.Combine(folder, "feature.en.vtt");
            await File.WriteAllTextAsync(subtitle, "WEBVTT\n\n00:00.000 --> 00:10.000\nHi\n");

            var provider = new SidecarWebVttTextSubtitleSource();
            using var canceled = new CancellationTokenSource();
            canceled.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => provider.DiscoverAsync(media, canceled.Token));
            var found = await provider.DiscoverAsync(media, CancellationToken.None);
            var selected = Assert.Single(found);
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => provider.LoadAsync(media, selected, canceled.Token));

            // Size cap applies to on-disk sidecars, not just parser input.
            await File.WriteAllTextAsync(subtitle, new string('x', 2_000_010));
            var tooLarge = await provider.LoadAsync(media, selected, CancellationToken.None);
            Assert.Null(tooLarge.At(TimeSpan.FromSeconds(1)));
        }
        finally { Directory.Delete(folder, recursive: true); }
    }

    [Fact]
    public async Task WebVttPreferredTrackFollowsCultureFallbackWithoutChangingAutoPolicy()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-vtt-language-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var media = Path.Combine(folder, "series.mp4");
            await File.WriteAllTextAsync(Path.Combine(folder, "series.vtt"),
                "WEBVTT\n\n00:00.000 --> 00:10.000\nGeneric\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "series.zh.vtt"),
                "WEBVTT\n\n00:00.000 --> 00:10.000\nChinese\n");
            var provider = new SidecarWebVttTextSubtitleSource();
            Assert.Equal("Chinese", (await provider.LoadPreferredAsync(media,
                CultureInfo.GetCultureInfo("zh-TW"), CancellationToken.None))
                .At(TimeSpan.FromSeconds(1)));
            Assert.Equal("Generic", (await provider.LoadPreferredAsync(media,
                CultureInfo.GetCultureInfo("fr-FR"), CancellationToken.None))
                .At(TimeSpan.FromSeconds(1)));
        }
        finally { Directory.Delete(folder, recursive: true); }
    }
}
