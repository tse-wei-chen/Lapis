using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class LibraryDropClassifierTests
{
    [Fact]
    public void MixedFilesAndDiscRootAreClassifiedAndDeduplicated()
    {
        using var files = new Fixture();
        var video = files.File("concert.MKV");
        var extra = files.File("clip.mp4");
        var root = files.CreateBdmv("Chronicle");
        var plan = LibraryDropClassifier.Classify([
            video, Path.Combine(root, "BDMV"), root, extra, video
        ]);
        Assert.Empty(plan.Rejected);
        Assert.Equal(3, plan.Sources.Count);
        Assert.Equal(MediaLocatorKind.File, plan.Sources[0].Kind);
        Assert.Equal(Path.GetFullPath(video), plan.Sources[0].Value);
        Assert.Equal(MediaLocatorKind.Directory, plan.Sources[1].Kind);
        Assert.Equal(Path.GetFullPath(root), plan.Sources[1].Value);
        Assert.Equal(MediaLocatorKind.File, plan.Sources[2].Kind);
    }

    [Fact]
    public void InvalidFilesAndUnrelatedDirectoriesCannotBecomeFakeMovies()
    {
        using var files = new Fixture();
        var invalidDir = Path.Combine(files.Root, "random");
        Directory.CreateDirectory(invalidDir);
        var picture = files.File("RSL_wakareD1_L.jpg");
        var playlist = files.File("00001.mpls");
        var iso = files.File("movie.iso");
        var video = files.File("film.m2ts");

        var result = LibraryDropClassifier.Classify([
            invalidDir, picture, playlist, iso, Path.Combine(files.Root, "missing"),
            video, null
        ]);

        Assert.Equal(new[] { MediaLocatorKind.File },
            result.Sources.Select(source => source.Kind));
        Assert.Equal(Path.GetFullPath(video), result.Sources[0].Value);
        Assert.Equal(6, result.Rejected.Count);
    }

    [Fact]
    public void LimitsDropBatchWithoutRecursiveDirectoryScanning()
    {
        using var files = new Fixture();
        var clip = files.File("film.mp4");
        var plan = LibraryDropClassifier.Classify(
            Enumerable.Repeat<string?>(clip, LibraryDropClassifier.MaxDroppedItems + 20));
        Assert.Single(plan.Sources);
        Assert.Single(plan.Rejected);
    }

    private sealed class Fixture : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(),
            "lapis-drop-" + Guid.NewGuid().ToString("N"));

        public Fixture() => Directory.CreateDirectory(Root);
        public string File(string name)
        {
            var path = Path.Combine(Root, name);
            System.IO.File.WriteAllText(path, "test");
            return path;
        }
        public string CreateBdmv(string name)
        {
            var root = Path.Combine(Root, name);
            var bdmv = Path.Combine(root, "BDMV");
            Directory.CreateDirectory(Path.Combine(bdmv, "PLAYLIST"));
            Directory.CreateDirectory(Path.Combine(bdmv, "STREAM"));
            Directory.CreateDirectory(Path.Combine(root, "CERTIFICATE"));
            Directory.CreateDirectory(Path.Combine(root, "MAKEMKV", "AACS"));
            System.IO.File.WriteAllText(Path.Combine(bdmv, "index.bdmv"), "test");
            return root;
        }
        public void Dispose()
        {
            if (Directory.Exists(Root)) Directory.Delete(Root, recursive: true);
        }
    }
}
