using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Lapis.Contracts.Media;
using Lapis.Library;

namespace Lapis.Desktop.Avalonia.ViewModels;

public enum LibraryDisplayMode { Grid, List }
public enum LibraryCategory { All, BluRay, Videos, RecentlyAdded }
public enum LibrarySortMode { RecentlyAdded, TitleAscending, TitleDescending }

public sealed record LibrarySortChoice(LibrarySortMode Mode, string Label);

/// <summary>
/// Collection-first shell state. Both visual layouts use exactly the same card
/// identities/filtered collection; no native runtime is created while browsing.
/// </summary>
public sealed class LibraryViewModel(IMediaLibrary library) : INotifyPropertyChanged, IDisposable
{
    private readonly IMediaLibrary _library = library;
    private readonly Dictionary<Guid, LibraryCardViewModel> _cards = [];
    private IReadOnlyList<MediaLibraryItem> _items = [];
    private bool _disposed;
    private string _searchText = "";
    private string _statusText = "選取收藏項目以播放；原始影片不會被搬移。";
    private LibraryCategory _category;
    private LibraryDisplayMode _displayMode = LibraryDisplayMode.Grid;
    private LibrarySortChoice _selectedSort = SortChoices[0];
    private bool _loaded;

    public event PropertyChangedEventHandler? PropertyChanged;
    public ObservableCollection<LibraryCardViewModel> VisibleItems { get; } = [];

    public static IReadOnlyList<LibrarySortChoice> SortChoices { get; } =
    [
        new(LibrarySortMode.RecentlyAdded, "最近加入"),
        new(LibrarySortMode.TitleAscending, "片名 A–Z"),
        new(LibrarySortMode.TitleDescending, "片名 Z–A")
    ];

    public IReadOnlyList<LibrarySortChoice> SortOptions => SortChoices;

    public string SearchText
    {
        get => _searchText;
        set
        {
            value ??= "";
            if (_searchText == value) return;
            _searchText = value;
            Notify();
            ApplyFilter();
        }
    }

    public LibrarySortChoice SelectedSort
    {
        get => _selectedSort;
        set
        {
            if (value is null || _selectedSort == value) return;
            _selectedSort = value;
            Notify();
            ApplyFilter();
        }
    }

    public string StatusText
    {
        get => _statusText;
        private set
        {
            if (_statusText == value) return;
            _statusText = value;
            Notify();
        }
    }

    public LibraryDisplayMode DisplayMode
    {
        get => _displayMode;
        private set
        {
            if (_displayMode == value) return;
            _displayMode = value;
            Notify();
            Notify(nameof(IsGridView));
            Notify(nameof(IsListView));
        }
    }

    public bool IsGridView => DisplayMode == LibraryDisplayMode.Grid;
    public bool IsListView => DisplayMode == LibraryDisplayMode.List;
    public LibraryCategory Category => _category;
    public bool IsAllCategory => _category == LibraryCategory.All;
    public bool IsBluRayCategory => _category == LibraryCategory.BluRay;
    public bool IsVideoCategory => _category == LibraryCategory.Videos;
    public bool IsRecentlyAddedCategory => _category == LibraryCategory.RecentlyAdded;
    public bool IsRecentlyAddedSort => SelectedSort.Mode == LibrarySortMode.RecentlyAdded;

    public string CountText => $"{_items.Count} 部收藏 · {_items.Count(item => item.IsDisc)} 張光碟";
    public string ResultText => $"{VisibleItems.Count} 部影片";
    public bool HasItems => _items.Count != 0;
    public bool IsEmpty => _loaded && _items.Count == 0;
    public bool HasNoResults => _loaded && _items.Count != 0 && VisibleItems.Count == 0;
    public string CurrentFilterLabel => _category switch
    {
        LibraryCategory.BluRay => "Blu-ray 光碟",
        LibraryCategory.Videos => "本機影片",
        LibraryCategory.RecentlyAdded => "最近加入",
        _ => "所有收藏"
    };

    public async Task LoadAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        var items = await _library.ListAsync(cancellationToken);
        ObjectDisposedException.ThrowIf(_disposed, this);
        _items = items;
        var identities = _items.Select(item => item.Id).ToHashSet();
        foreach (var stale in _cards.Keys.Where(key => !identities.Contains(key)).ToArray())
        {
            _cards[stale].Dispose();
            _cards.Remove(stale);
        }
        foreach (var item in _items)
            _cards.TryAdd(item.Id, new LibraryCardViewModel(item));
        _loaded = true;
        Notify(nameof(CountText));
        Notify(nameof(HasItems));
        ApplyFilter();
    }

    public async Task ImportAsync(MediaLocator source, CancellationToken cancellationToken = default)
    {
        var item = await _library.ImportAsync(source, cancellationToken);
        await LoadAsync(cancellationToken);
        StatusText = $"已加入收藏：{item.Title}（未複製原始影片）";
    }

    public async Task RemoveAsync(MediaLibraryItem item, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(item);
        if (await _library.RemoveAsync(item.Id, cancellationToken))
        {
            await LoadAsync(cancellationToken);
            StatusText = $"已移出收藏庫：{item.Title}（原始檔案未刪除）";
        }
    }

    public void ShowAll() => SetCategory(LibraryCategory.All);
    public void ShowDiscs() => SetCategory(LibraryCategory.BluRay);
    public void ShowVideos() => SetCategory(LibraryCategory.Videos);
    public void ShowRecentlyAdded()
    {
        SetCategory(LibraryCategory.RecentlyAdded);
        SelectedSort = SortChoices[0];
    }

    public void ShowGrid() => DisplayMode = LibraryDisplayMode.Grid;
    public void ShowList() => DisplayMode = LibraryDisplayMode.List;
    public void ReportError(string message) => StatusText = message;

    private void SetCategory(LibraryCategory category)
    {
        if (_category == category) return;
        _category = category;
        Notify(nameof(IsAllCategory));
        Notify(nameof(IsBluRayCategory));
        Notify(nameof(IsVideoCategory));
        Notify(nameof(IsRecentlyAddedCategory));
        Notify(nameof(Category));
        ApplyFilter();
    }

    private void ApplyFilter()
    {
        if (_disposed) return;
        var needle = _searchText.Trim();
        IEnumerable<MediaLibraryItem> filtered = _items.Where(item =>
            (_category is LibraryCategory.All or LibraryCategory.RecentlyAdded ||
             (_category == LibraryCategory.BluRay && item.IsDisc) ||
             (_category == LibraryCategory.Videos && !item.IsDisc)) &&
            (needle.Length == 0 ||
             item.Title.Contains(needle, StringComparison.OrdinalIgnoreCase) ||
             item.SourcePath.Contains(needle, StringComparison.OrdinalIgnoreCase)));

        filtered = SelectedSort.Mode switch
        {
            LibrarySortMode.TitleAscending => filtered
                .OrderBy(item => item.Title, StringComparer.OrdinalIgnoreCase)
                .ThenBy(item => item.Id),
            LibrarySortMode.TitleDescending => filtered
                .OrderByDescending(item => item.Title, StringComparer.OrdinalIgnoreCase)
                .ThenBy(item => item.Id),
            _ => filtered.OrderByDescending(item => item.ImportedAtUtc)
                .ThenBy(item => item.Id)
        };

        // Preserve visual container identities for unchanged cards instead
        // of clearing every row on each search/sort/category action. A Reset
        // forces the ItemsControl to rebuild the entire visual tree and
        // interrupts ongoing compositor transitions.
        var desired = filtered.Select(item => _cards[item.Id]).ToArray();
        for (var index = 0; index < desired.Length; index++)
        {
            var card = desired[index];
            if (index >= VisibleItems.Count || !ReferenceEquals(VisibleItems[index], card))
            {
                var previous = VisibleItems.IndexOf(card);
                if (previous >= 0)
                    VisibleItems.Move(previous, index);
                else
                    VisibleItems.Insert(index, card);
            }
            card.EnsurePosterLoaded();
        }
        while (VisibleItems.Count > desired.Length)
            VisibleItems.RemoveAt(VisibleItems.Count - 1);
        Notify(nameof(CurrentFilterLabel));
        Notify(nameof(ResultText));
        Notify(nameof(IsEmpty));
        Notify(nameof(HasNoResults));
        Notify(nameof(IsRecentlyAddedSort));
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var card in _cards.Values) card.Dispose();
        _cards.Clear();
        VisibleItems.Clear();
    }

    private void Notify([CallerMemberName] string property = "") =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
}
