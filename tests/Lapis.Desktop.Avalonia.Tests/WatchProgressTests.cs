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
    public async Task FinishedMovieLeavesWatchHistoryButNotAResumeBookmark()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-history-" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(folder, "progress.json");
        try
        {
            var store = new JsonWatchProgressStore(path);
            var id = Guid.NewGuid();
            var start = new WatchProgress(id, 100, 240, DateTimeOffset.UtcNow);
            await store.SaveAsync(start);
            Assert.Single(await store.LoadAsync());
            Assert.Single(await store.LoadHistoryAsync());

            var finished = start with { PositionSeconds = 239,
                UpdatedAtUtc = start.UpdatedAtUtc.AddSeconds(10) };
            Assert.True(finished.IsCompleted);
            Assert.False(finished.IsResumable);
            await store.SaveAsync(finished);
            Assert.Empty(await store.LoadAsync());
            Assert.Equal(finished, Assert.Single(await store.LoadHistoryAsync()));

            await store.ForgetAsync(id);
            Assert.Empty(await new JsonWatchProgressStore(path).LoadHistoryAsync());
            Assert.Empty(await new JsonWatchProgressStore(path).LoadAsync());
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        }
    }

    [Fact]
    public async Task LegacyVersionOneSidecarLoadsAndUpgradesWithoutDiscardingBookmarks()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-history-" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(folder, "progress.json");
        Directory.CreateDirectory(folder);
        try
        {
            var first = new WatchProgress(Guid.NewGuid(), 80, 240, DateTimeOffset.UtcNow);
            var original = System.Text.Json.JsonSerializer.Serialize(
                new { version = 1, items = new[] { first } });
            await File.WriteAllTextAsync(path, original);
            var store = new JsonWatchProgressStore(path);
            Assert.Equal(first, Assert.Single(await store.LoadAsync()));
            Assert.Equal(first, Assert.Single(await store.LoadHistoryAsync()));

            var second = new WatchProgress(Guid.NewGuid(), 75, 220, DateTimeOffset.UtcNow);
            await store.SaveAsync(second);
            Assert.Equal(2, (await store.LoadAsync()).Count);
            Assert.Equal(2, (await store.LoadHistoryAsync()).Count);
            Assert.Contains("\"version\": 2", await File.ReadAllTextAsync(path));
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        }
    }

    [Fact]
    public async Task ShortViewingDoesNotEraseEarlierBookmarkOrHistory()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-history-" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(folder, "progress.json");
        try
        {
            var store = new JsonWatchProgressStore(path);
            var saved = new WatchProgress(Guid.NewGuid(), 80, 240, DateTimeOffset.UtcNow);
            await store.SaveAsync(saved);
            await store.SaveAsync(saved with { PositionSeconds = 5 });
            Assert.Equal(saved, Assert.Single(await store.LoadAsync()));
            Assert.Equal(saved, Assert.Single(await store.LoadHistoryAsync()));
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
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
