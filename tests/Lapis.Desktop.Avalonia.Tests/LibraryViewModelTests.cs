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
        view.ShowList();
        Assert.True(view.IsListView);
        Assert.False(view.IsGridView);
        Assert.Same(item, Assert.Single(view.VisibleItems));

        view.ShowGrid();
        Assert.True(view.IsGridView);
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
