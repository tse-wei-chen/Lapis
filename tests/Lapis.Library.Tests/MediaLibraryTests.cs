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
    public async Task TypicalMakeMkvBackupLayoutIsOneDiscWithPrimaryPlaylistsOnly()
    {
        using var test = new TestFiles();
        var root = test.CreateDisc("Wakare");
        var bdmv = Path.Combine(root, "BDMV");
        Directory.CreateDirectory(Path.Combine(bdmv, "AUXDATA"));
        Directory.CreateDirectory(Path.Combine(bdmv, "BACKUP", "PLAYLIST"));
        Directory.CreateDirectory(Path.Combine(bdmv, "BACKUP", "CLIPINF"));
        Directory.CreateDirectory(Path.Combine(bdmv, "BDJO"));
        Directory.CreateDirectory(Path.Combine(bdmv, "JAR"));
        Directory.CreateDirectory(Path.Combine(bdmv, "CLIPINF"));
        Directory.CreateDirectory(Path.Combine(bdmv, "META", "DL"));
        Directory.CreateDirectory(Path.Combine(root, "CERTIFICATE", "BACKUP"));
        Directory.CreateDirectory(Path.Combine(root, "MAKEMKV", "AACS", "DUPLICATE"));
        Directory.CreateDirectory(Path.Combine(root, "MAKEMKV", "CMAP"));
        File.WriteAllText(Path.Combine(bdmv, "PLAYLIST", "00002.mpls"), "");
        File.WriteAllText(Path.Combine(bdmv, "PLAYLIST", "00004.mpls"), "");
        File.WriteAllText(Path.Combine(bdmv, "BACKUP", "PLAYLIST", "00999.mpls"), "");
        File.WriteAllText(Path.Combine(bdmv, "META", "DL", "bdmt_jpn.xml"), "<disc/>");
        File.WriteAllBytes(Path.Combine(bdmv, "META", "DL", "RSL_wakareD1_L.jpg"), [0xFF, 0xD8, 0xFF]);
        File.WriteAllText(Path.Combine(root, "MAKEMKV", "AACS", "MKB_RO.inf"), "");
        File.WriteAllText(Path.Combine(root, "MAKEMKV", "discattd.dat"), "");
        var library = new JsonMediaLibrary(Path.Combine(test.Root, "catalog.json"));

        var disc = await library.ImportAsync(
            new MediaLocator(MediaLocatorKind.Directory, root));
        var byBdmv = await library.ImportAsync(
            new MediaLocator(MediaLocatorKind.Directory, bdmv));

        Assert.Equal(disc.Id, byBdmv.Id);
        Assert.Equal(root, disc.SourcePath);
        Assert.Equal(new[] { "00002", "00004" },
            DiscPlaylistIndex.ListIdentifiers(disc));
        Assert.Single(await library.ListAsync());
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

    [Fact]
    public async Task MovedMovieRelinkPreservesIdentityFavoritesAndWatchHistory()
    {
        using var test = new TestFiles();
        var oldPath = Path.Combine(test.Root, "original.mp4");
        var movedPath = Path.Combine(test.Root, "new-drive.mp4");
        File.WriteAllText(oldPath, "original media bytes");
        var catalogPath = Path.Combine(test.Root, "catalog.json");
        var library = new JsonMediaLibrary(catalogPath);
        var original = await library.ImportAsync(new MediaLocator(MediaLocatorKind.File, oldPath));
        var favorite = new JsonFavoriteStore(catalogPath + ".favorites.v1.json");
        var history = new JsonWatchProgressStore(catalogPath + ".watch.v2.json");
        await favorite.SetAsync(original.Id, true);
        var watch = new WatchProgress(original.Id, 75, 240, DateTimeOffset.UtcNow);
        await history.SaveAsync(watch);
        File.Move(oldPath, movedPath);
        Assert.False(original.IsAvailable);

        var fixedMovie = await library.RelinkAsync(original.Id,
            new MediaLocator(MediaLocatorKind.File, movedPath));
        Assert.Equal(original.Id, fixedMovie.Id);
        Assert.Equal(original.Title, fixedMovie.Title);
        Assert.Equal(original.ImportedAtUtc, fixedMovie.ImportedAtUtc);
        Assert.Equal(movedPath, fixedMovie.SourcePath);
        Assert.True(fixedMovie.IsAvailable);
        Assert.Equal(fixedMovie, Assert.Single(
            await new JsonMediaLibrary(catalogPath).ListAsync()));
        Assert.Equal(original.Id, Assert.Single(await favorite.LoadAsync()).ItemId);
        Assert.Equal(watch, Assert.Single(await history.LoadHistoryAsync()));
        Assert.True(File.Exists(movedPath));
        Assert.False(File.Exists(oldPath));
    }

    [Fact]
    public async Task RelinkRejectsAvailableSourceDifferentKindAndDuplicateTarget()
    {
        using var test = new TestFiles();
        var oldPath = Path.Combine(test.Root, "old.mp4");
        var targetPath = Path.Combine(test.Root, "target.mp4");
        File.WriteAllText(oldPath, "original");
        File.WriteAllText(targetPath, "other");
        var catalogPath = Path.Combine(test.Root, "catalog.json");
        var library = new JsonMediaLibrary(catalogPath);
        var old = await library.ImportAsync(new MediaLocator(MediaLocatorKind.File, oldPath));
        var other = await library.ImportAsync(new MediaLocator(MediaLocatorKind.File, targetPath));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            library.RelinkAsync(old.Id, new MediaLocator(MediaLocatorKind.File, targetPath)));
        File.Delete(oldPath);
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            library.RelinkAsync(old.Id, new MediaLocator(MediaLocatorKind.File, targetPath)));

        var disc = test.CreateDisc("replacement");
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            library.RelinkAsync(old.Id, new MediaLocator(MediaLocatorKind.Directory, disc)));
        await Assert.ThrowsAsync<KeyNotFoundException>(() =>
            library.RelinkAsync(Guid.NewGuid(), new MediaLocator(MediaLocatorKind.File, targetPath)));
        Assert.Equal(new[] { old, other }, await library.ListAsync());
        Assert.Equal(2, (await new JsonMediaLibrary(catalogPath).ListAsync()).Count);
    }

    [Fact]
    public async Task RelinkDiscNormalizesBdmvChildAndRejectsIncompleteDisc()
    {
        using var test = new TestFiles();
        var source = test.CreateDisc("disc-original");
        var replacement = test.CreateDisc("disc-remounted");
        var library = new JsonMediaLibrary(Path.Combine(test.Root, "catalog.json"));
        var item = await library.ImportAsync(new MediaLocator(MediaLocatorKind.Directory, source));
        Directory.Delete(source, recursive: true);
        var repaired = await library.RelinkAsync(item.Id, new MediaLocator(
            MediaLocatorKind.Directory, Path.Combine(replacement, "BDMV")));
        Assert.Equal(replacement, repaired.SourcePath);
        Assert.Equal(item.Id, repaired.Id);
        Assert.Single(await library.ListAsync());
        Assert.True(repaired.IsAvailable);

        var broken = Path.Combine(test.Root, "empty-folder");
        Directory.CreateDirectory(broken);
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            library.RelinkAsync(item.Id, new MediaLocator(MediaLocatorKind.Directory, broken)));
        Assert.Equal(repaired, Assert.Single(await library.ListAsync()));
    }

    [Fact]
    public async Task RenameMovieChangesOnlyDisplayMetadataAndSurvivesReload()
    {
        using var test = new TestFiles();
        var file = Path.Combine(test.Root, "Movie.2026.1080p.mkv");
        File.WriteAllText(file, "original media remains");
        var catalogPath = Path.Combine(test.Root, "catalog.json");
        var catalog = new JsonMediaLibrary(catalogPath);
        var movie = await catalog.ImportAsync(new MediaLocator(MediaLocatorKind.File, file));

        var edited = await catalog.RenameAsync(movie.Id, "  My Cinema Title  ");
        Assert.Equal("My Cinema Title", edited.Title);
        Assert.Equal(movie.Id, edited.Id);
        Assert.Equal(movie.SourcePath, edited.SourcePath);
        Assert.Equal(movie.Kind, edited.Kind);
        Assert.Equal(movie.ImportedAtUtc, edited.ImportedAtUtc);
        Assert.True(File.Exists(file));

        var reloaded = Assert.Single(await new JsonMediaLibrary(catalogPath).ListAsync());
        Assert.Equal(edited, reloaded);
        var idempotent = await catalog.RenameAsync(movie.Id, "My Cinema Title");
        Assert.Equal(edited, idempotent);
    }

    [Fact]
    public async Task RenameValidatesTitleAndNeverClobbersExistingCatalog()
    {
        using var test = new TestFiles();
        var file = Path.Combine(test.Root, "Movie.mp4");
        File.WriteAllText(file, "fixture");
        var catalogPath = Path.Combine(test.Root, "catalog.json");
        var catalog = new JsonMediaLibrary(catalogPath);
        var movie = await catalog.ImportAsync(new MediaLocator(MediaLocatorKind.File, file));
        var originalBytes = await File.ReadAllBytesAsync(catalogPath);

        foreach (var title in new[] { "", " \t ", "newline\nnot allowed",
                     new string('x', 161), "bad\u0001control" })
            await Assert.ThrowsAsync<ArgumentException>(() => catalog.RenameAsync(movie.Id, title));
        await Assert.ThrowsAsync<KeyNotFoundException>(() =>
            catalog.RenameAsync(Guid.NewGuid(), "Missing movie"));
        Assert.Equal(originalBytes, await File.ReadAllBytesAsync(catalogPath));
        Assert.Equal(movie, Assert.Single(await catalog.ListAsync()));
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
