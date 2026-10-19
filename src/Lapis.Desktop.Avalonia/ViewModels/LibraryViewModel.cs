using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Lapis.Contracts.Media;
using Lapis.Library;

namespace Lapis.Desktop.Avalonia.ViewModels;

/// <summary>
/// Library-first home view state, entirely independent from native playback.
/// The library's import operation indexes sources and never starts playback.
/// </summary>
public sealed class LibraryViewModel(IMediaLibrary library) : INotifyPropertyChanged
{
    private readonly IMediaLibrary _library = library;
    private IReadOnlyList<MediaLibraryItem> _items = [];
    private string _searchText = "";
    private string _statusText = "收藏你的電影；點選封面才開始播放。";
    private bool _discsOnly;
    private bool _loaded;

    public event PropertyChangedEventHandler? PropertyChanged;
    public ObservableCollection<MediaLibraryItem> VisibleItems { get; } = [];

    public string SearchText
    {
        get => _searchText;
        set
        {
            if (_searchText == value) return;
            _searchText = value ?? "";
            OnPropertyChanged();
            ApplyFilter();
        }
    }

    public string StatusText
    {
        get => _statusText;
        private set
        {
            _statusText = value;
            OnPropertyChanged();
        }
    }

    public string CountText => $"{_items.Count} 部收藏 · {_items.Count(item => item.IsDisc)} 張光碟";
    public bool HasItems => _items.Count != 0;
    public bool IsEmpty => _loaded && _items.Count == 0;
    public bool HasNoResults => _loaded && _items.Count != 0 && VisibleItems.Count == 0;
    public string CurrentFilterLabel => _discsOnly ? "藍光光碟" : "全部影片";

    public async Task LoadAsync(CancellationToken cancellationToken = default)
    {
        _items = await _library.ListAsync(cancellationToken);
        _loaded = true;
        OnPropertyChanged(nameof(CountText));
        OnPropertyChanged(nameof(HasItems));
        ApplyFilter();
    }

    public async Task ImportAsync(MediaLocator source, CancellationToken cancellationToken = default)
    {
        var item = await _library.ImportAsync(source, cancellationToken);
        await LoadAsync(cancellationToken);
        StatusText = $"已加入收藏：{item.Title}（不會複製原始影片）";
    }

    public async Task RemoveAsync(MediaLibraryItem item, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(item);
        if (await _library.RemoveAsync(item.Id, cancellationToken))
        {
            await LoadAsync(cancellationToken);
            StatusText = $"已移出收藏庫：{item.Title}（原始檔案不受影響）";
        }
    }

    public void ShowAll() { _discsOnly = false; ApplyFilter(); }
    public void ShowDiscs() { _discsOnly = true; ApplyFilter(); }
    public void ReportError(string message) => StatusText = message;

    private void ApplyFilter()
    {
        var needle = _searchText.Trim();
        VisibleItems.Clear();
        foreach (var item in _items.OrderByDescending(item => item.ImportedAtUtc))
        {
            if (_discsOnly && !item.IsDisc) continue;
            if (needle.Length != 0 &&
                !item.Title.Contains(needle, StringComparison.OrdinalIgnoreCase) &&
                !item.SourcePath.Contains(needle, StringComparison.OrdinalIgnoreCase))
                continue;
            VisibleItems.Add(item);
        }
        OnPropertyChanged(nameof(IsEmpty));
        OnPropertyChanged(nameof(HasNoResults));
        OnPropertyChanged(nameof(CurrentFilterLabel));
    }

    private void OnPropertyChanged([CallerMemberName] string property = "") =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
}
