using Lapis.Library;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class WatchProgressTests
{
    [Fact]
    public async Task JournalPersistsResumableTitlesAndRemovesFinishedPlayback()
    {
        var path = Path.Combine(Path.GetTempPath(),
            "lapis-watch-" + Guid.NewGuid().ToString("N"), "progress.json");
        try
        {
            var store = new JsonWatchProgressStore(path);
            var film = Guid.NewGuid();
            var saved = new WatchProgress(film, 110, 240, DateTimeOffset.UtcNow);
            Assert.True(saved.IsResumable);
            await store.SaveAsync(saved);

            // Reload from disk, not the in-memory journal.
            var reloaded = new JsonWatchProgressStore(path);
            Assert.Equal(saved, Assert.Single(await reloaded.LoadAsync()));
            await reloaded.SaveAsync(saved with { PositionSeconds = 239 });
            Assert.Empty(await new JsonWatchProgressStore(path).LoadAsync());

            // Ineligible short clips and unstarted sessions stay out of the shelf.
            await store.SaveAsync(new WatchProgress(Guid.NewGuid(), 15, 240, DateTimeOffset.UtcNow));
            await store.SaveAsync(new WatchProgress(Guid.NewGuid(), 20, 40, DateTimeOffset.UtcNow));
            Assert.Empty(await store.LoadAsync());
        }
        finally
        {
            if (Directory.Exists(Path.GetDirectoryName(path)!))
                Directory.Delete(Path.GetDirectoryName(path)!, recursive: true);
        }
    }

    [Fact]
    public async Task InvalidJournalDoesNotGetOverwrittenBySave()
    {
        var path = Path.Combine(Path.GetTempPath(),
            "lapis-watch-" + Guid.NewGuid().ToString("N"), "progress.json");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            await File.WriteAllTextAsync(path, "{bad-json");
            var store = new JsonWatchProgressStore(path);
            await Assert.ThrowsAsync<InvalidDataException>(() =>
                store.SaveAsync(new WatchProgress(Guid.NewGuid(), 80, 200, DateTimeOffset.UtcNow)));
            Assert.Equal("{bad-json", await File.ReadAllTextAsync(path));
        }
        finally
        {
            if (Directory.Exists(Path.GetDirectoryName(path)!))
                Directory.Delete(Path.GetDirectoryName(path)!, recursive: true);
        }
    }
}
