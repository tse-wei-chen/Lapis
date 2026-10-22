using Lapis.Contracts.Media;
using Lapis.Library;
using Xunit;

namespace Lapis.Library.Tests;

public sealed class LocalArtworkLocatorTests
{
    [Fact]
    public void PrefersMovieSpecificSidecarsOverGenericFolderCovers()
    {
        using var directory = new Fixture();
        var movie = directory.Movie("A Summer Film.mkv");
        var poster = directory.Image("A Summer Film.poster.JPG", "poster");
        directory.Image("A Summer Film.png", "still");
        directory.Image("folder.jpg", "folder");
        var candidates = LocalArtworkLocator.FindCandidates(movie);
        Assert.Equal(poster, candidates[0]);
        Assert.Equal(3, candidates.Count);
    }

    [Fact]
    public void NeverAssignsGenericFolderCoverToMultipleFilms()
    {
        using var directory = new Fixture();
        var first = directory.Movie("first.mp4");
        var second = directory.Movie("second.mkv");
        directory.Image("poster.jpg", "common");
        var specific = directory.Image("first.cover.png", "specific");
        Assert.Equal(specific, Assert.Single(LocalArtworkLocator.FindCandidates(first)));
        Assert.Empty(LocalArtworkLocator.FindCandidates(second));
    }

    [Fact]
    public void HandlesMissingFilesOversizedSourcesAndUntrustedExtensions()
    {
        using var directory = new Fixture();
        var movie = directory.Movie("clip.mp4");
        directory.Image("clip.poster.jpeg", ""); // empty source
        directory.Image("clip.jpg", "image bytes");
        directory.Image("clip.exe", "not an image");
        using (var huge = File.Create(Path.Combine(directory.Root, "clip.cover.png")))
            huge.SetLength(LocalArtworkLocator.MaxSourceBytes + 1);
        Assert.Equal("clip.jpg", Path.GetFileName(
            Assert.Single(LocalArtworkLocator.FindCandidates(movie))));

        File.Delete(movie.SourcePath);
        Assert.Empty(LocalArtworkLocator.FindCandidates(movie));
    }

    [Fact]
    public void SupportsSingleFilmFolderCoverWithoutChangingDiscContract()
    {
        using var directory = new Fixture();
        var movie = directory.Movie("clip.mp4");
        var cover = directory.Image("folder.PNG", "image");
        Assert.Equal(cover, Assert.Single(LocalArtworkLocator.FindCandidates(movie)));
        var disc = movie with { Kind = MediaLocatorKind.Directory };
        Assert.Empty(LocalArtworkLocator.FindCandidates(disc));
    }

    [Fact]
    public void RefreshDiscoveryRecognizesAddedReplacedAndDeletedArtwork()
    {
        using var directory = new Fixture();
        var movie = directory.Movie("Weekend.mkv");
        Assert.Empty(LocalArtworkLocator.FindCandidates(movie));

        var cover = directory.Image("Weekend.poster.jpg", "first image");
        Assert.Equal(cover, Assert.Single(LocalArtworkLocator.FindCandidates(movie)));

        File.WriteAllText(cover, "replacement image");
        Assert.Equal(cover, Assert.Single(LocalArtworkLocator.FindCandidates(movie)));

        File.Delete(cover);
        Assert.Empty(LocalArtworkLocator.FindCandidates(movie));
    }

    private sealed class Fixture : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(),
            "lapis-local-art-" + Guid.NewGuid().ToString("N"));
        public Fixture() => Directory.CreateDirectory(Root);

        public MediaLibraryItem Movie(string file)
        {
            var path = Path.Combine(Root, file);
            File.WriteAllText(path, "media");
            return new MediaLibraryItem(Guid.NewGuid(),
                MediaLocatorKind.File, path, Path.GetFileNameWithoutExtension(file),
                DateTimeOffset.UtcNow);
        }

        public string Image(string file, string content)
        {
            var path = Path.Combine(Root, file);
            File.WriteAllText(path, content);
            return path;
        }

        public void Dispose()
        {
            if (Directory.Exists(Root)) Directory.Delete(Root, recursive: true);
        }
    }
}
