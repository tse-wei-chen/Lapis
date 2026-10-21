using System.Globalization;
using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class SidecarSubtitleTrackTests
{
    [Fact]
    public void ParsesSimpleSrtWithTransparentTimedCues()
    {
        var track = SidecarSubtitleTrack.Parse(
            "1\r\n00:00:01,000 --> 00:00:02,500\r\n第一句字幕\r\n\r\n" +
            "2\n00:00:03.000 --> 00:00:04.000\nSecond line\n下一行\n");
        Assert.Equal(2, track.Count);
        Assert.Null(track.At(TimeSpan.FromMilliseconds(500)));
        Assert.Equal("第一句字幕", track.At(TimeSpan.FromSeconds(1.2)));
        Assert.Null(track.At(TimeSpan.FromSeconds(2.5)));
        Assert.Equal("Second line\n下一行", track.At(TimeSpan.FromSeconds(3.1)));
        Assert.Null(track.At(TimeSpan.FromSeconds(4.1)));
    }

    [Fact]
    public void IgnoresMalformedOutOfOrderOrOversizedCues()
    {
        var track = SidecarSubtitleTrack.Parse(
            "1\n00:00:05,000 --> 00:00:04,000\nInvalid\n\n" +
            "2\n00:00:02,000 --> 00:00:03,000\nTwo\n\n" +
            "3\n00:00:00,500 --> 00:00:01,000\nOne\n");
        Assert.Equal(2, track.Count);
        Assert.Equal("One", track.At(TimeSpan.FromMilliseconds(800)));
        Assert.Equal("Two", track.At(TimeSpan.FromMilliseconds(2200)));
    }

    [Fact]
    public async Task SidecarFileIsOptionalAndDoesNotAlterMovie()
    {
        var root = Path.Combine(Path.GetTempPath(), "lapis-subtitle-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var file = Path.Combine(root, "film.mp4");
            await File.WriteAllTextAsync(file, "video fixture");
            Assert.Equal(0, (await SidecarSubtitleTrack.LoadForFileAsync(file)).Count);
            await File.WriteAllTextAsync(Path.Combine(root, "film.srt"),
                "1\n00:00:01,000 --> 00:00:02,000\n你好\n");
            var track = await SidecarSubtitleTrack.LoadForFileAsync(file);
            Assert.Equal("你好", track.At(TimeSpan.FromSeconds(1.5)));
            Assert.Equal("video fixture", await File.ReadAllTextAsync(file));
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    [Fact]
    public void NestedOverlapsKeepEarlierCueVisibleAfterLaterCueExpires()
    {
        // Sorted by start does NOT mean sorted by end.
        // Previously a query at 5s could return null instead of "Long".
        var track = SidecarSubtitleTrack.Parse(
            "1\n00:00:00,000 --> 00:00:10,000\nLong\n\n" +
            "2\n00:00:01,000 --> 00:00:02,000\nShort\n\n" +
            "3\n00:00:03,000 --> 00:00:04,000\nLater\n");

        Assert.Equal(3, track.Count);
        Assert.Null(track.At(TimeSpan.FromMilliseconds(-1)));
        Assert.Equal("Long", track.At(TimeSpan.Zero));
        Assert.Equal("Short", track.At(TimeSpan.FromSeconds(1.5)));
        Assert.Equal("Long", track.At(TimeSpan.FromSeconds(2))); // exclusive End
        Assert.Equal("Later", track.At(TimeSpan.FromSeconds(3.5)));
        Assert.Equal("Long", track.At(TimeSpan.FromSeconds(4)));
        Assert.Equal("Long", track.At(TimeSpan.FromSeconds(9.999)));
        Assert.Null(track.At(TimeSpan.FromSeconds(10)));
    }

    [Fact]
    public void FindsMostRecentStillActiveCueAcrossOutOfOrderCuesAndSeeks()
    {
        var track = SidecarSubtitleTrack.Parse(
            "1\n00:00:03,000 --> 00:00:04,000\nNewest\n\n" +
            "2\n00:00:00,000 --> 00:00:10,000\nBackground\n\n" +
            "3\n00:00:02,000 --> 00:00:08,000\nMiddle\n");

        // Seek back/forth over overlapping and nested intervals.
        Assert.Equal("Middle", track.At(TimeSpan.FromSeconds(5)));
        Assert.Equal("Newest", track.At(TimeSpan.FromSeconds(3.5)));
        Assert.Equal("Middle", track.At(TimeSpan.FromSeconds(4)));
        Assert.Equal("Background", track.At(TimeSpan.FromSeconds(8)));
        Assert.Equal("Middle", track.At(TimeSpan.FromSeconds(2)));
        Assert.Equal("Background", track.At(TimeSpan.FromSeconds(1)));
        Assert.Null(track.At(TimeSpan.FromSeconds(11)));
    }

    [Fact]
    public void EqualStartTimesPreferLastCueAndFallBackWhenItEnds()
    {
        var track = SidecarSubtitleTrack.Parse(
            "1\n00:00:01,000 --> 00:00:03,000\nFirst\n\n" +
            "2\n00:00:01,000 --> 00:00:02,000\nSecond\n");

        Assert.Equal("Second", track.At(TimeSpan.FromSeconds(1.5)));
        Assert.Equal("First", track.At(TimeSpan.FromSeconds(2)));
        Assert.Null(track.At(TimeSpan.FromSeconds(3)));
    }

    [Fact]
    public async Task LocalizedSidecarsPreferExactLanguageThenParentThenUntagged()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-language-subtitles-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var movie = Path.Combine(folder, "film.mp4");
            await File.WriteAllTextAsync(Path.Combine(folder, "film.srt"),
                "1\n00:00:01,000 --> 00:00:02,000\nDefault\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "film.zh.srt"),
                "1\n00:00:01,000 --> 00:00:02,000\nChinese\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "film.zh-TW.srt"),
                "1\n00:00:01,000 --> 00:00:02,000\n繁體中文\n");
            await File.WriteAllTextAsync(Path.Combine(folder, "film.ja-JP.srt"),
                "1\n00:00:01,000 --> 00:00:02,000\n日本語\n");

            static string? AtOneSecond(SidecarSubtitleTrack t) =>
                t.At(TimeSpan.FromSeconds(1.5));

            Assert.Equal("繁體中文", AtOneSecond(
                await SidecarSubtitleTrack.LoadForFileAsync(movie,
                    CultureInfo.GetCultureInfo("zh-TW"))));
            Assert.Equal("日本語", AtOneSecond(
                await SidecarSubtitleTrack.LoadForFileAsync(movie,
                    CultureInfo.GetCultureInfo("ja-JP"))));
            File.Delete(Path.Combine(folder, "film.zh-TW.srt"));
            Assert.Equal("Chinese", AtOneSecond(
                await SidecarSubtitleTrack.LoadForFileAsync(movie,
                    CultureInfo.GetCultureInfo("zh-TW"))));
            File.Delete(Path.Combine(folder, "film.zh.srt"));
            Assert.Equal("Default", AtOneSecond(
                await SidecarSubtitleTrack.LoadForFileAsync(movie,
                    CultureInfo.GetCultureInfo("zh-TW"))));
            Assert.Equal("Default", AtOneSecond(
                await SidecarSubtitleTrack.LoadForFileAsync(movie,
                    CultureInfo.GetCultureInfo("fr-FR"))));
        }
        finally { Directory.Delete(folder, recursive: true); }
    }

    [Fact]
    public async Task InvalidPreferredSubtitleDoesNotSuppressValidFallback()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-language-fallback-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var movie = Path.Combine(folder, "title.mkv");
            await File.WriteAllTextAsync(Path.Combine(folder, "title.ja-JP.srt"),
                "not an SRT cue");
            await File.WriteAllTextAsync(Path.Combine(folder, "title.srt"),
                "1\n00:00:00,000 --> 00:00:10,000\nFallback\n");
            var track = await SidecarSubtitleTrack.LoadForFileAsync(
                movie, CultureInfo.GetCultureInfo("ja-JP"));
            Assert.Equal("Fallback", track.At(TimeSpan.FromSeconds(3)));
        }
        finally { Directory.Delete(folder, recursive: true); }
    }

    [Fact]
    public async Task CancelledSidecarLoadDoesNotBecomeAnEmptyTrack()
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            SidecarSubtitleTrack.LoadForFileAsync(
                Path.Combine(Path.GetTempPath(), "cancelled.mp4"),
                CultureInfo.GetCultureInfo("zh-TW"), cancellation.Token));
    }

    [Fact]
    public async Task DiscoversOnlyThisMovieSidecarsWithStableLanguageChoices()
    {
        var root = Path.Combine(Path.GetTempPath(),
            "lapis-subtitle-picker-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var video = Path.Combine(root, "film.mp4");
            foreach (var name in new[]
            {
                "film.srt", "film.ja-JP.srt", "film.zh-TW.srt",
                "other.zh-TW.srt", "film.zh_TW.srt", "film.en.ass",
                "filming.en.srt", "film.zh-TW.backup.srt"
            })
                await File.WriteAllTextAsync(Path.Combine(root, name),
                    "1\n00:00:00,000 --> 00:00:02,000\nSubtitle\n");

            var options = await SidecarSubtitleTrack.DiscoverForFileAsync(video);
            Assert.Equal(new[] { "film.ja-JP.srt", "film.srt", "film.zh-TW.srt" },
                options.Select(option => option.DisplayName).ToArray());
            Assert.All(options, option => Assert.Equal(root,
                Path.GetDirectoryName(option.FilePath)));
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    [Fact]
    public async Task ExplicitSelectionUsesChosenLanguageWithoutAutoFallback()
    {
        var root = Path.Combine(Path.GetTempPath(),
            "lapis-subtitle-explicit-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var video = Path.Combine(root, "movie.mkv");
            var defaultFile = Path.Combine(root, "movie.srt");
            var jaFile = Path.Combine(root, "movie.ja-JP.srt");
            await File.WriteAllTextAsync(defaultFile,
                "1\n00:00:00,000 --> 00:00:02,000\nDefault\n");
            await File.WriteAllTextAsync(jaFile,
                "1\n00:00:00,000 --> 00:00:02,000\nJapanese\n");
            var options = await SidecarSubtitleTrack.DiscoverForFileAsync(video);
            var selected = Assert.Single(options,
                file => file.DisplayName == "movie.ja-JP.srt");
            var loaded = await SidecarSubtitleTrack.LoadExactFileAsync(selected.FilePath);
            Assert.Equal("Japanese", loaded.At(TimeSpan.FromSeconds(1)));
            File.Delete(jaFile);
            Assert.Equal(0, (await SidecarSubtitleTrack.LoadExactFileAsync(jaFile)).Count);
            Assert.Equal("Default", (await SidecarSubtitleTrack.LoadForFileAsync(
                video, CultureInfo.GetCultureInfo("ja-JP"))).At(TimeSpan.FromSeconds(1)));
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    [Fact]
    public async Task DiscoversAtMost32ChoicesAndHonorsCancellation()
    {
        var root = Path.Combine(Path.GetTempPath(),
            "lapis-subtitle-cap-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var video = Path.Combine(root, "series.mkv");
            for (var i = 0; i < 50; ++i)
                await File.WriteAllTextAsync(
                    Path.Combine(root, $"series.language{i:00}.srt"), "");
            var options = await SidecarSubtitleTrack.DiscoverForFileAsync(video);
            Assert.Equal(32, options.Count);

            using var cancelled = new CancellationTokenSource();
            cancelled.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => SidecarSubtitleTrack.DiscoverForFileAsync(video, cancelled.Token));
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}
