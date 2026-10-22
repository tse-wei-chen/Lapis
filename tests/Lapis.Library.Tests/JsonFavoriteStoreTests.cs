using Lapis.Library;
using Xunit;

namespace Lapis.Library.Tests;

public sealed class JsonFavoriteStoreTests
{
    [Fact]
    public async Task FavoritesPersistAcrossInstancesAndUnpinDoesNotChangeOtherEntries()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-favorite-" + Guid.NewGuid().ToString("N"));
        var file = Path.Combine(folder, "favorites.json");
        try
        {
            var first = Guid.NewGuid();
            var second = Guid.NewGuid();
            var writer = new JsonFavoriteStore(file);
            await writer.SetAsync(first, true);
            await writer.SetAsync(second, true);

            var before = await new JsonFavoriteStore(file).LoadAsync();
            Assert.Equal(2, before.Count);
            var original = before.Single(x => x.ItemId == first);
            await writer.SetAsync(first, true); // Idempotent; original pinned date.
            Assert.Equal(original, (await writer.LoadAsync()).Single(x => x.ItemId == first));
            await writer.SetAsync(second, false);
            await writer.SetAsync(second, false);

            Assert.Equal(original,
                Assert.Single(await new JsonFavoriteStore(file).LoadAsync()));
            await writer.SetAsync(first, false);
            Assert.Empty(await writer.LoadAsync());
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        }
    }

    [Theory]
    [InlineData("{broken-json")]
    [InlineData("{\"version\":99,\"items\":[]}")]
    public async Task InvalidFileIsPreservedWhenAttemptingToWrite(string invalid)
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-favorite-" + Guid.NewGuid().ToString("N"));
        var file = Path.Combine(folder, "favorites.json");
        Directory.CreateDirectory(folder);
        try
        {
            await File.WriteAllTextAsync(file, invalid);
            var store = new JsonFavoriteStore(file);
            await Assert.ThrowsAsync<InvalidDataException>(
                () => store.SetAsync(Guid.NewGuid(), true));
            Assert.Equal(invalid, await File.ReadAllTextAsync(file));
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        }
    }

    [Fact]
    public async Task MissingFileReturnsEmptyAndInvalidIdCannotBePinned()
    {
        var file = Path.Combine(Path.GetTempPath(),
            "lapis-no-favorite-" + Guid.NewGuid().ToString("N"), "favorites.json");
        var store = new JsonFavoriteStore(file);
        Assert.Empty(await store.LoadAsync());
        await Assert.ThrowsAsync<ArgumentException>(() => store.SetAsync(Guid.Empty, true));
        Assert.False(File.Exists(file));
    }
}
