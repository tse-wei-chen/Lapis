using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.ViewModels;
using Lapis.Library;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class LibraryViewModelTests
{
    [Fact]
    public async Task StartsEmptyAndImportsWithoutStartingPlayback()
    {
        var storage = new FakeLibrary();
        var view = new LibraryViewModel(storage);
        Assert.Empty(view.VisibleItems);
        await view.LoadAsync();
        Assert.True(view.IsEmpty);
        Assert.Equal("0 部收藏 · 0 張光碟", view.CountText);

        await view.ImportAsync(new MediaLocator(MediaLocatorKind.Directory, @"C:\Movies\Disc"));
        Assert.Single(view.VisibleItems);
        Assert.False(view.IsEmpty);
        Assert.Equal("1 部收藏 · 1 張光碟", view.CountText);
        Assert.Equal("Blu-ray", view.VisibleItems[0].Title);

        view.ShowDiscs();
        Assert.Single(view.VisibleItems);
        view.SearchText = "not in title";
        Assert.Empty(view.VisibleItems);
        Assert.True(view.HasNoResults);
        view.SearchText = "blu";
        Assert.Single(view.VisibleItems);
    }

    [Fact]
    public async Task GridAndListShareTheSameCardsWithoutStartingPlayback()
    {
        var storage = new FakeLibrary();
        using var view = new LibraryViewModel(storage);
        await view.LoadAsync();
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, @"C:\Videos\film.mkv"));
        var item = Assert.Single(view.VisibleItems);

        Assert.Equal(LibraryDisplayMode.Grid, view.DisplayMode);
        Assert.True(view.IsGridView);
        Assert.False(view.IsListView);
        Assert.True(view.ShowFeaturedHero);
        Assert.True(view.ShowRecentShelf);

        view.ShowList();
        Assert.True(view.IsListView);
        Assert.False(view.IsGridView);
        Assert.False(view.ShowFeaturedHero);
        Assert.False(view.ShowRecentShelf);
        Assert.False(view.ShowFavoritesShelf);
        Assert.False(view.ShowContinueShelf);
        Assert.False(view.ShowHistoryShelf);
        Assert.Same(item, Assert.Single(view.VisibleItems));

        view.ShowGrid();
        Assert.True(view.IsGridView);
        Assert.True(view.ShowFeaturedHero);
        Assert.True(view.ShowRecentShelf);
        Assert.Same(item, Assert.Single(view.VisibleItems));
        Assert.Equal(1, storage.ImportCount);
    }

    [Fact]
    public async Task SearchSortAndCategoryStayConsistentAcrossViews()
    {
        var storage = new FakeLibrary();
        using var view = new LibraryViewModel(storage);
        await view.LoadAsync();
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "Zebra.mkv"));
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.Directory, "BluRay"));
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "Alpha.mp4"));

        Assert.Equal(3, view.VisibleItems.Count);
        view.SelectedSort = LibraryViewModel.SortChoices[1];
        Assert.Equal(new[] { "Alpha", "Blu-ray", "Zebra" },
            view.VisibleItems.Select(card => card.Title).ToArray());
        view.ShowVideos();
        Assert.True(view.IsVideoCategory);
        Assert.Equal(new[] { "Alpha", "Zebra" },
            view.VisibleItems.Select(card => card.Title).ToArray());

        view.ShowList();
        view.SearchText = "zebra";
        Assert.Equal("1 部影片", view.ResultText);
        Assert.Equal("Zebra", Assert.Single(view.VisibleItems).Title);
        view.ShowRecentlyAdded();
        Assert.Equal(LibraryDisplayMode.List, view.DisplayMode);
        Assert.False(view.IsAllCategory);
        Assert.True(view.IsRecentlyAddedCategory);
        Assert.Equal(LibraryCategory.RecentlyAdded, view.Category);
        Assert.Equal("最近加入", view.CurrentFilterLabel);
        Assert.True(view.IsRecentlyAddedSort);
        Assert.Single(view.VisibleItems); // Search preserved.
        view.SearchText = "";
        Assert.Equal(new[] { "Alpha", "Blu-ray", "Zebra" },
            view.VisibleItems.Select(card => card.Title).ToArray());
    }

    [Fact]
    public async Task SortingAndFilteringMoveExistingCardsWithoutResettingCollection()
    {
        var storage = new FakeLibrary();
        using var view = new LibraryViewModel(storage);
        await view.LoadAsync();
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "Zebra.mkv"));
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "Alpha.mkv"));
        var cards = view.VisibleItems.ToDictionary(card => card.Title);
        var actions = new List<System.Collections.Specialized.NotifyCollectionChangedAction>();
        view.VisibleItems.CollectionChanged += (_, change) => actions.Add(change.Action);

        view.SelectedSort = LibraryViewModel.SortChoices[1];
        Assert.Equal(new[] { "Alpha", "Zebra" },
            view.VisibleItems.Select(card => card.Title));
        Assert.Same(cards["Alpha"], view.VisibleItems[0]);
        Assert.Same(cards["Zebra"], view.VisibleItems[1]);
        view.SearchText = "Zebra";
        Assert.Same(cards["Zebra"], Assert.Single(view.VisibleItems));
        view.SearchText = "";
        Assert.Equal(2, view.VisibleItems.Count);
        Assert.DoesNotContain(System.Collections.Specialized.NotifyCollectionChangedAction.Reset, actions);
    }

    [Fact]
    public async Task BigPictureFeaturedAndRecentShelvesShareCatalogIdentity()
    {
        var storage = new FakeLibrary();
        using var view = new LibraryViewModel(storage);
        await view.LoadAsync();
        Assert.Null(view.FeaturedCard);

        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "Alpha.mp4"));
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.Directory, "BluRay"));
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "Zebra.mkv"));

        Assert.Equal(3, view.RecentItems.Count);
        Assert.Contains(view.FeaturedCard!, view.VisibleItems);
        Assert.Equal("Alpha", view.FeaturedCard!.Title); // Import preserves user selection.
        var candidate = view.VisibleItems[1];
        view.SelectFeatured(candidate);
        Assert.Same(candidate, view.FeaturedCard);
        view.ShowDiscs();
        Assert.Equal("Blu-ray", Assert.Single(view.VisibleItems).Title);
        Assert.Single(view.RecentItems);
        Assert.Equal("Blu-ray", view.FeaturedCard!.Title);
        view.ShowAll();
        Assert.Equal(3, view.RecentItems.Count);
        Assert.Same(view.VisibleItems.Single(x => x.Title == "Blu-ray"),
            view.FeaturedCard);
    }

    [Fact]
    public async Task ContinueWatchingShelfTracksOnlyResumableAvailableFilms()
    {
        var file = Path.Combine(Path.GetTempPath(), "lapis-watch-test-" + Guid.NewGuid() + ".mp4");
        await File.WriteAllTextAsync(file, "fake media content");
        try
        {
            using var view = new LibraryViewModel(new FakeLibrary());
            await view.LoadAsync();
            await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, file));
            var card = Assert.Single(view.VisibleItems);
            var progress = new WatchProgress(card.Item.Id, 80, 240, DateTimeOffset.UtcNow);

            view.LoadWatchProgress([progress]);
            Assert.True(view.HasContinueWatching);
            Assert.Same(card, Assert.Single(view.ContinueWatchingItems));
            Assert.Equal(80, card.ResumePositionSeconds);
            Assert.Equal("▶  繼續播放", card.PlayActionLabel);

            view.SearchText = "nothing";
            Assert.False(view.HasContinueWatching);
            view.SearchText = "";
            Assert.True(view.HasContinueWatching);
            view.ShowDiscs();
            Assert.False(view.HasContinueWatching);
            view.ShowAll();
            Assert.True(view.HasContinueWatching);

            view.ApplyWatchSnapshot(progress with
            {
                PositionSeconds = 239,
                UpdatedAtUtc = progress.UpdatedAtUtc.AddSeconds(1)
            });
            Assert.Empty(view.ContinueWatchingItems);
            Assert.False(card.CanResume);
        }
        finally
        {
            File.Delete(file);
        }
    }

    [Fact]
    public async Task OutOfOrderUiContinuationsCannotRewindHistoryOrResume()
    {
        var root = Path.Combine(Path.GetTempPath(), "lapis-ui-order-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var file = Path.Combine(root, "film.mp4");
            File.WriteAllText(file, "fixture");
            using var view = new LibraryViewModel(new JsonMediaLibrary(
                Path.Combine(root, "catalog.json")));
            await view.LoadAsync();
            await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, file));
            var card = Assert.Single(view.VisibleItems);
            var initial = new WatchProgress(card.Item.Id, 80, 240, DateTimeOffset.UtcNow);
            var complete = initial with
            {
                PositionSeconds = 239,
                UpdatedAtUtc = initial.UpdatedAtUtc.AddSeconds(10)
            };

            // A newer completion is accepted first.
            view.ApplyWatchSnapshot(complete);
            Assert.False(card.CanResume);
            Assert.True(card.IsWatched);

            // An old continuation running *after* it must not resurrect Resume.
            view.ApplyWatchSnapshot(initial);
            Assert.False(card.CanResume);
            Assert.True(card.IsWatched);
            Assert.Same(card, Assert.Single(view.RecentlyWatchedItems));

            var fresh = initial with
            {
                PositionSeconds = 95,
                UpdatedAtUtc = initial.UpdatedAtUtc.AddMinutes(1)
            };
            view.ApplyWatchSnapshot(fresh);
            Assert.True(card.CanResume);
            Assert.False(card.IsWatched);
            view.ApplyWatchSnapshot(complete);
            Assert.True(card.CanResume);
            Assert.Equal(95, card.ResumePositionSeconds);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task WatchHistoryFiltersAndSortsCompletedTitlesWithoutMakingThemResumable()
    {
        var folder = Path.Combine(Path.GetTempPath(),
            "lapis-history-view-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        var first = Path.Combine(folder, "first.mp4");
        var second = Path.Combine(folder, "second.mp4");
        await File.WriteAllTextAsync(first, "fixture");
        await File.WriteAllTextAsync(second, "fixture");
        try
        {
            using var view = new LibraryViewModel(new FakeLibrary());
            await view.LoadAsync();
            await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, first));
            await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, second));
            var cards = view.VisibleItems.ToDictionary(item => item.Title);
            var now = DateTimeOffset.UtcNow;
            var watching = new WatchProgress(cards["first"].Item.Id, 80, 240, now.AddDays(-1));
            var finished = new WatchProgress(cards["second"].Item.Id, 239, 240, now);
            view.LoadWatchProgress([watching]);
            view.LoadWatchHistory([watching, finished]);

            Assert.True(view.HasContinueWatching);
            Assert.True(view.HasRecentlyWatched);
            Assert.Equal("✓ 已看完", cards["second"].HistoryLabel);
            Assert.False(cards["second"].CanResume);

            view.ShowHistory();
            Assert.True(view.IsHistoryCategory);
            Assert.Equal("觀看紀錄", view.CurrentFilterLabel);
            Assert.Equal(new[] { "second", "first" },
                view.VisibleItems.Select(item => item.Title));
            Assert.False(view.HasContinueWatching);
            Assert.False(view.HasRecentlyWatched);

            view.ForgetWatchHistory(finished.ItemId);
            Assert.Equal("first", Assert.Single(view.VisibleItems).Title);
            Assert.False(cards["second"].HasWatchHistory);
            view.ShowAll();
            Assert.True(view.HasRecentlyWatched);
            Assert.Single(view.RecentlyWatchedItems);
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        }
    }

    [Fact]
    public async Task FavoritesPersistAsCuratedCategoryAndHomeShelfWithoutPlayingMovies()
    {
        using var view = new LibraryViewModel(new FakeLibrary());
        await view.LoadAsync();
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "First.mp4"));
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.Directory, "BDMV"));
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, "Second.mkv"));

        var cards = view.VisibleItems.ToDictionary(x => x.Title);
        var first = cards["First"];
        var disc = cards["Blu-ray"];
        var now = DateTimeOffset.UtcNow;
        view.LoadFavorites([
            new FavoriteEntry(first.Item.Id, now.AddDays(-1)),
            new FavoriteEntry(disc.Item.Id, now)
        ]);
        Assert.True(view.HasFavorites);
        Assert.Equal(2, view.FavoriteItems.Count);
        Assert.True(first.IsFavorite);
        Assert.True(disc.IsFavorite);
        Assert.False(cards["Second"].IsFavorite);
        Assert.Equal("★  移出我的最愛", disc.FavoriteActionLabel);
        Assert.Same(disc, view.FavoriteItems[0]); // Recently pinned first.

        view.ShowFavorites();
        Assert.True(view.IsFavoritesCategory);
        Assert.Equal("我的最愛", view.CurrentFilterLabel);
        Assert.Equal(new[] { "Blu-ray", "First" },
            view.VisibleItems.Select(x => x.Title));
        Assert.False(view.HasFavorites); // Shelf belongs to the All home only.

        view.SearchText = "First";
        Assert.Same(first, Assert.Single(view.VisibleItems));
        view.SearchText = "";
        view.SetFavorite(disc.Item.Id, false);
        Assert.Same(first, Assert.Single(view.VisibleItems));
        Assert.False(disc.IsFavorite);
        view.ShowAll();
        Assert.True(view.HasFavorites);
        Assert.Same(first, Assert.Single(view.FavoriteItems));
        Assert.Equal(3, view.VisibleItems.Count); // Media was never removed.
    }

    [Fact]
    public async Task SourceRepairReplacesStaleCardButKeepsLogicalFavoriteAndResumeState()
    {
        var root = Path.Combine(Path.GetTempPath(), "lapis-relink-view-" + Guid.NewGuid());
        Directory.CreateDirectory(root);
        try
        {
            var oldPath = Path.Combine(root, "favorite.mp4");
            var newPath = Path.Combine(root, "favorite-moved.mp4");
            File.WriteAllText(oldPath, "original");
            using var view = new LibraryViewModel(
                new JsonMediaLibrary(Path.Combine(root, "catalog.json")));
            await view.LoadAsync();
            await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, oldPath));
            var oldCard = Assert.Single(view.VisibleItems);
            var now = DateTimeOffset.UtcNow;
            view.LoadFavorites([new FavoriteEntry(oldCard.Item.Id, now)]);
            view.LoadWatchProgress([new WatchProgress(oldCard.Item.Id, 90, 240, now)]);
            File.Move(oldPath, newPath);
            view.ShowFavorites();
            Assert.True(oldCard.IsFavorite);
            Assert.False(oldCard.IsAvailable);

            await view.RelinkAsync(oldCard.Item, new MediaLocator(MediaLocatorKind.File, newPath));
            var card = Assert.Single(view.VisibleItems);
            Assert.NotSame(oldCard, card); // Poster loader must pick up new sidecar.
            Assert.Equal(oldCard.Item.Id, card.Item.Id);
            Assert.Equal(oldCard.Title, card.Title);
            Assert.Equal(newPath, card.SourcePath);
            Assert.True(card.IsFavorite);
            Assert.True(card.CanResume);
            Assert.True(card.IsAvailable);
            view.ShowAll();
            Assert.Same(card, Assert.Single(view.FavoriteItems));
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public async Task RenameTitleRefreshesSearchSortAndRetainsFavoriteAndBookmark()
    {
        var folder = Path.Combine(Path.GetTempPath(), "lapis-title-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var first = Path.Combine(folder, "zebra.2026.mkv");
            var second = Path.Combine(folder, "alpha.mp4");
            File.WriteAllText(first, "media");
            File.WriteAllText(second, "media");
            var catalogPath = Path.Combine(folder, "catalog.json");
            using var view = new LibraryViewModel(new JsonMediaLibrary(catalogPath));
            await view.LoadAsync();
            await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, first));
            await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, second));
            var card = Assert.Single(view.VisibleItems, x => x.Title == "zebra.2026");
            var id = card.Item.Id;
            var now = DateTimeOffset.UtcNow;
            view.LoadFavorites([new FavoriteEntry(id, now)]);
            view.LoadWatchProgress([new WatchProgress(id, 70, 240, now)]);
            Assert.True(card.IsFavorite);
            Assert.True(card.CanResume);

            await view.RenameAsync(card.Item, "A Favorite Movie");
            var renamed = Assert.Single(view.VisibleItems, x => x.Title == "A Favorite Movie");
            Assert.Equal(id, renamed.Item.Id);
            Assert.True(renamed.IsFavorite);
            Assert.True(renamed.CanResume);
            view.SelectedSort = LibraryViewModel.SortChoices[1];
            Assert.Equal(new[] { "A Favorite Movie", "alpha" },
                view.VisibleItems.Select(x => x.Title));
            view.SearchText = "Favorite";
            Assert.Same(renamed, Assert.Single(view.VisibleItems));
            view.ShowFavorites();
            Assert.Same(renamed, Assert.Single(view.VisibleItems));
            Assert.Equal("A Favorite Movie",
                (await new JsonMediaLibrary(catalogPath).ListAsync())
                .Single(x => x.Id == id).Title);
            Assert.True(File.Exists(first));
        }
        finally { Directory.Delete(folder, recursive: true); }
    }

    [Fact]
    public async Task SidebarNavigationMaintainsExactlyOneActiveTarget()
    {
        using var view = new LibraryViewModel(new FakeLibrary());
        await view.LoadAsync();
        Assert.True(view.IsAllCategory);
        Assert.False(view.IsRecentlyAddedCategory);

        view.ShowDiscs();
        Assert.True(view.IsBluRayCategory);
        Assert.False(view.IsAllCategory);

        view.ShowRecentlyAdded();
        Assert.True(view.IsRecentlyAddedCategory);
        Assert.False(view.IsBluRayCategory);
        Assert.False(view.IsAllCategory);

        view.ShowAll();
        Assert.True(view.IsAllCategory);
        Assert.False(view.IsRecentlyAddedCategory);
    }

    [Fact]
    public async Task BulkDropImportPreservesExistingItemsAndOnlyIndexesSources()
    {
        var directory = Path.Combine(Path.GetTempPath(), "lapis-bulk-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        try
        {
            var clip = Path.Combine(directory, "trailer.mp4");
            File.WriteAllText(clip, "original stays unchanged");
            var root = Path.Combine(directory, "BluRayMovie");
            Directory.CreateDirectory(Path.Combine(root, "BDMV", "PLAYLIST"));
            Directory.CreateDirectory(Path.Combine(root, "BDMV", "STREAM"));
            File.WriteAllText(Path.Combine(root, "BDMV", "index.bdmv"), "");
            using var view = new LibraryViewModel(
                new JsonMediaLibrary(Path.Combine(directory, "catalog.json")));
            await view.LoadAsync();

            var movie = new MediaLocator(MediaLocatorKind.File, clip);
            var disc = new MediaLocator(MediaLocatorKind.Directory, root);
            var first = await view.ImportManyAsync([movie, disc], rejectedCount: 2);
            Assert.Equal(new LibraryDropImportResult(2, 0, 2), first);
            Assert.Equal(2, view.VisibleItems.Count);
            Assert.True(view.IsAllCategory);

            view.ShowDiscs();
            view.SearchText = "not-a-title";
            var again = await view.ImportManyAsync([movie, disc]);
            Assert.Equal(new LibraryDropImportResult(0, 2, 0), again);
            Assert.True(view.IsAllCategory);
            Assert.Equal("", view.SearchText);
            Assert.Equal(2, view.VisibleItems.Count);
            Assert.Equal("original stays unchanged", File.ReadAllText(clip));
            Assert.True(File.Exists(Path.Combine(root, "BDMV", "index.bdmv")));
        }
        finally
        {
            if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public async Task RemoveOnlyUpdatesCatalogAndFiltering()
    {
        var storage = new FakeLibrary();
        var view = new LibraryViewModel(storage);
        await view.LoadAsync();
        await view.ImportAsync(new MediaLocator(MediaLocatorKind.File, @"C:\Videos\film.mkv"));
        var entry = Assert.Single(view.VisibleItems);
        view.ShowDiscs();
        Assert.Empty(view.VisibleItems);
        view.ShowAll();
        Assert.Single(view.VisibleItems);
        await view.RemoveAsync(entry.Item);
        Assert.True(view.IsEmpty);
        Assert.Empty(view.VisibleItems);
        Assert.Single(storage.RemovedIds);
    }

    private sealed class FakeLibrary : IMediaLibrary
    {
        private readonly List<MediaLibraryItem> _items = [];
        public List<Guid> RemovedIds { get; } = [];
        public int ImportCount { get; private set; }
        public Task<IReadOnlyList<MediaLibraryItem>> ListAsync(
            CancellationToken cancellationToken = default) =>
            Task.FromResult<IReadOnlyList<MediaLibraryItem>>(_items.ToArray());

        public Task<MediaLibraryItem> ImportAsync(MediaLocator locator,
            CancellationToken cancellationToken = default)
        {
            ImportCount++;
            var result = new MediaLibraryItem(
                Guid.NewGuid(), locator.Kind, locator.Value,
                locator.Kind == MediaLocatorKind.Directory ? "Blu-ray" :
                Path.GetFileNameWithoutExtension(locator.Value) == "film" ? "Film" :
                Path.GetFileNameWithoutExtension(locator.Value),
                DateTimeOffset.UnixEpoch.AddMinutes(ImportCount));
            _items.Add(result);
            return Task.FromResult(result);
        }

        public Task<bool> RemoveAsync(Guid id, CancellationToken cancellationToken = default)
        {
            RemovedIds.Add(id);
            return Task.FromResult(_items.RemoveAll(item => item.Id == id) > 0);
        }
    }
}
