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
        public Task<IReadOnlyList<MediaLibraryItem>> ListAsync(
            CancellationToken cancellationToken = default) =>
            Task.FromResult<IReadOnlyList<MediaLibraryItem>>(_items.ToArray());

        public Task<MediaLibraryItem> ImportAsync(MediaLocator locator,
            CancellationToken cancellationToken = default)
        {
            var result = new MediaLibraryItem(
                Guid.NewGuid(), locator.Kind, locator.Value,
                locator.Kind == MediaLocatorKind.Directory ? "Blu-ray" : "Film",
                DateTimeOffset.UtcNow);
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
