using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.Controls;
using Lapis.Desktop.Avalonia.Services;
using Lapis.Desktop.Avalonia.ViewModels;
using Lapis.Library;

namespace Lapis.Desktop.Avalonia.Views;

/// <summary>
/// Two explicitly separate screens: library first, Native watch only after
/// selecting a catalog item. NativeControlHost never sits under a fake
/// translucent Avalonia video overlay.
/// </summary>
public sealed partial class MainWindow : Window
{
    private readonly WindowsVideoSurface _videoSurface;
    private readonly Slider _timeline;
    private readonly Grid _libraryPage;
    private readonly Grid _playerPage;
    private readonly Border _discMenuPanel;
    private readonly ItemsControl _discPlaylistList;
    private readonly PlayerViewModel _player;
    private readonly LibraryViewModel _library;
    private bool _closeStarted;
    private bool _shutdownComplete;

    public MainWindow()
    {
        InitializeComponent();
        _libraryPage = this.FindControl<Grid>("LibraryPage")
            ?? throw new InvalidOperationException("Missing library page.");
        _playerPage = this.FindControl<Grid>("PlayerPage")
            ?? throw new InvalidOperationException("Missing watch page.");
        _discMenuPanel = this.FindControl<Border>("DiscMenuPanel")
            ?? throw new InvalidOperationException("Missing disc navigation panel.");
        _discPlaylistList = this.FindControl<ItemsControl>("DiscPlaylistList")
            ?? throw new InvalidOperationException("Missing disc playlist index.");
        _videoSurface = this.FindControl<WindowsVideoSurface>("VideoSurface")
            ?? throw new InvalidOperationException("Missing D3D11 surface host.");
        _timeline = this.FindControl<Slider>("PlaybackTimeline")
            ?? throw new InvalidOperationException("Missing playback timeline.");

        var libraryFile = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Lapis", "Library", "catalog.v1.json");
        _library = new LibraryViewModel(new JsonMediaLibrary(libraryFile));
        _player = new PlayerViewModel(
            new NativePlayerPlaybackService(() => _videoSurface.OutputHandle));
        _libraryPage.DataContext = _library;
        _playerPage.DataContext = _player;

        // No player source, Native DLL or video pipeline is initialized at startup.
        _libraryPage.IsVisible = true;
        _playerPage.IsVisible = false;
        Opened += async (_, _) =>
        {
            try { await _library.LoadAsync(); }
            catch (Exception exception) { _library.ReportError($"無法讀取收藏庫：{exception.Message}"); }
        };

        _timeline.AddHandler(InputElement.PointerPressedEvent,
            TimelinePointerPressed, RoutingStrategies.Tunnel, handledEventsToo: true);
        _timeline.AddHandler(InputElement.PointerReleasedEvent,
            TimelinePointerReleased, RoutingStrategies.Tunnel, handledEventsToo: true);
        _timeline.AddHandler(InputElement.PointerCaptureLostEvent,
            TimelinePointerCaptureLost, RoutingStrategies.Bubble, handledEventsToo: true);
        Closing += OnClosing;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void TimelinePointerPressed(object? sender, PointerPressedEventArgs args)
    {
        if (args.GetCurrentPoint(_timeline).Properties.IsLeftButtonPressed)
            _player.BeginScrub();
    }

    private async void TimelinePointerReleased(object? sender, PointerReleasedEventArgs args) =>
        await _player.CommitScrubAsync();

    private void TimelinePointerCaptureLost(object? sender, PointerCaptureLostEventArgs args) =>
        _player.CancelScrub();

    private async void ImportFiles_Click(object? sender, RoutedEventArgs args)
    {
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                Title = "匯入本機影片至收藏庫",
                AllowMultiple = true
            });
            foreach (var file in files)
            {
                var path = file.TryGetLocalPath();
                if (string.IsNullOrWhiteSpace(path))
                {
                    _library.ReportError("僅能加入本機影片，不能匯入遠端 URI。");
                    continue;
                }
                await _library.ImportAsync(new MediaLocator(MediaLocatorKind.File, path));
            }
        }
        catch (Exception exception)
        {
            _library.ReportError($"匯入影片失敗：{exception.Message}");
        }
    }

    private async void ImportDisc_Click(object? sender, RoutedEventArgs args)
    {
        try
        {
            var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            {
                Title = "匯入完整 Blu-ray 光碟根目錄（包含 BDMV）",
                AllowMultiple = false
            });
            if (folders.Count == 0) return;
            var path = folders[0].TryGetLocalPath();
            if (!BdmvLocator.TryFromDirectory(path, out var locator))
            {
                _library.ReportError("匯入失敗：需要完整 BDMV/index.bdmv、PLAYLIST 和 STREAM 資料夾。");
                return;
            }
            await _library.ImportAsync(locator!);
        }
        catch (Exception exception)
        {
            _library.ReportError($"匯入 BDMV 失敗：{exception.Message}");
        }
    }

    private void ShowAll_Click(object? sender, RoutedEventArgs args) => _library.ShowAll();
    private void ShowDiscs_Click(object? sender, RoutedEventArgs args) => _library.ShowDiscs();

    private async void PlayLibraryItem_Click(object? sender, RoutedEventArgs args)
    {
        if (sender is not Button { CommandParameter: MediaLibraryItem item }) return;
        try
        {
            // Validate that the referenced source still exists before switching
            // UI. Opening never copies a disc or treats .m2ts as a title.
            var source = LibrarySourcePaths.Normalize(item.ToLocator());

            _discMenuPanel.IsVisible = item.IsDisc;
            _discPlaylistList.ItemsSource = item.IsDisc
                ? DiscPlaylistIndex.ListIdentifiers(item)
                    .Select(id => id + ".mpls").ToArray()
                : [];
            _libraryPage.IsVisible = false;
            _playerPage.IsVisible = true;

            // A collapsed NativeControlHost does not necessarily have an HWND.
            // Render/loaded dispatch passes must finish before native Open binds
            // the borrowed output window. Do not block the UI dispatcher.
            await AwaitVideoSurfaceAsync();
            await _player.OpenAsync(source);
            if (!_player.CanTransport)
            {
                _library.ReportError(_player.StatusText);
                _playerPage.IsVisible = false;
                _libraryPage.IsVisible = true;
            }
        }
        catch (Exception exception)
        {
            _library.ReportError($"無法播放收藏項目：{exception.Message}");
            _playerPage.IsVisible = false;
            _libraryPage.IsVisible = true;
        }
    }

    private async Task AwaitVideoSurfaceAsync()
    {
        for (var retry = 0; retry < 30; retry++)
        {
            await Dispatcher.UIThread.InvokeAsync(() => { }, DispatcherPriority.Loaded);
            try
            {
                if (_videoSurface.OutputHandle != 0) return;
            }
            catch (InvalidOperationException)
            {
                // NativeControlHost has not yet attached to the visual tree.
            }
            await Task.Delay(20);
        }
        throw new InvalidOperationException("影片輸出視窗尚未建立，請重新開啟收藏項目。");
    }

    private async void BackToLibrary_Click(object? sender, RoutedEventArgs args)
    {
        // The normal actor Close/detach completes before hiding the watcher.
        await _player.CloseAsync();
        _playerPage.IsVisible = false;
        _libraryPage.IsVisible = true;
    }

    private async void OnClosing(object? sender, WindowClosingEventArgs args)
    {
        if (_shutdownComplete) return;
        args.Cancel = true;
        if (_closeStarted) return;
        _closeStarted = true;
        try
        {
            // Native render worker joins before the persistent GPU HWND dies.
            await _player.DisposeAsync();
            _videoSurface.ReleaseAfterPlaybackStopped();
            _shutdownComplete = true;
            Close();
        }
        catch (Exception exception)
        {
            _library.ReportError($"無法安全關閉視窗：{exception.Message}");
            _closeStarted = false;
        }
    }
}
