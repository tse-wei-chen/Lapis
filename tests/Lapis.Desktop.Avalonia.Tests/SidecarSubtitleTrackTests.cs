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
}
