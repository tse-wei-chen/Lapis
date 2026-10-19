using Lapis.Contracts.Media;
using Lapis.Library;
using Xunit;

namespace Lapis.Library.Tests;

public sealed class MediaLibraryTests
{
    [Fact]
    public async Task EntireDiscIsOnePersistentItemAndChildImportDeduplicates()
    {
        using var test = new TestFiles();
        var root = test.CreateDisc("Movie");
        File.WriteAllText(Path.Combine(root, "BDMV", "PLAYLIST", "00003.mpls"), "");
        File.WriteAllText(Path.Combine(root, "BDMV", "PLAYLIST", "00001.mpls"), "");
        var catalogPath = Path.Combine(test.Root, "catalog.json");
        var library = new JsonMediaLibrary(catalogPath);

        var one = await library.ImportAsync(new MediaLocator(MediaLocatorKind.Directory,
            Path.Combine(root, "BDMV")));
        var duplicate = await library.ImportAsync(new MediaLocator(MediaLocatorKind.Directory, root));
        Assert.Equal(one.Id, duplicate.Id);
        Assert.Equal(root, one.SourcePath);
        Assert.True(one.IsDisc);
        Assert.Equal(new[] { "00001", "00003" }, DiscPlaylistIndex.ListIdentifiers(one));

        var reloaded = await new JsonMediaLibrary(catalogPath).ListAsync();
        Assert.Single(reloaded);
        Assert.Equal(one, reloaded[0]);
        Assert.True(File.Exists(Path.Combine(root, "BDMV", "index.bdmv")));
    }

    [Fact]
    public async Task FileImportSearchMetadataAndRemoveOnlyCatalog()
    {
        using var test = new TestFiles();
        var movie = Path.Combine(test.Root, "some movie.mkv");
        File.WriteAllText(movie, "fixture");
        var library = new JsonMediaLibrary(Path.Combine(test.Root, "catalog.json"));
        var item = await library.ImportAsync(new MediaLocator(MediaLocatorKind.File, movie));
        Assert.Equal("some movie", item.Title);
        Assert.True(item.IsAvailable);
        Assert.Single(await library.ListAsync());
        Assert.True(await library.RemoveAsync(item.Id));
        Assert.Empty(await library.ListAsync());
        Assert.False(await library.RemoveAsync(item.Id));
        Assert.True(File.Exists(movie));
    }

    [Fact]
    public async Task InvalidDiscAndBrokenCatalogNeverBecomeFakeMovie()
    {
        using var test = new TestFiles();
        var library = new JsonMediaLibrary(Path.Combine(test.Root, "catalog.json"));
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            library.ImportAsync(new MediaLocator(MediaLocatorKind.Directory, test.Root)));
        Assert.Empty(await library.ListAsync());

        File.WriteAllText(Path.Combine(test.Root, "catalog.json"), "{ broken");
        var video = Path.Combine(test.Root, "film.mp4");
        File.WriteAllText(video, "fixture");
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            library.ImportAsync(new MediaLocator(MediaLocatorKind.File, video)));
        Assert.Equal("{ broken", File.ReadAllText(Path.Combine(test.Root, "catalog.json")));
    }

    private sealed class TestFiles : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(), "lapis-library-" + Guid.NewGuid());
        public TestFiles() => Directory.CreateDirectory(Root);
        public string CreateDisc(string name)
        {
            var disc = Path.Combine(Root, name);
            var bdmv = Path.Combine(disc, "BDMV");
            Directory.CreateDirectory(Path.Combine(bdmv, "PLAYLIST"));
            Directory.CreateDirectory(Path.Combine(bdmv, "STREAM"));
            File.WriteAllText(Path.Combine(bdmv, "index.bdmv"), "");
            return disc;
        }
        public void Dispose() { if (Directory.Exists(Root)) Directory.Delete(Root, recursive: true); }
    }
}
