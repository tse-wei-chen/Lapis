using System.Diagnostics;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Styling;
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
    private void ShowVideos_Click(object? sender, RoutedEventArgs args) => _library.ShowVideos();
    private void ShowRecentlyAdded_Click(object? sender, RoutedEventArgs args) =>
        _library.ShowRecentlyAdded();
    private void ShowGrid_Click(object? sender, RoutedEventArgs args) => _library.ShowGrid();
    private void ShowList_Click(object? sender, RoutedEventArgs args) => _library.ShowList();

    private async void PlayLibraryItem_Click(object? sender, RoutedEventArgs args)
    {
        if (sender is Button { CommandParameter: LibraryCardViewModel card })
            await PlayLibraryItemAsync(card);
    }

    private async void ContextPlay_Click(object? sender, RoutedEventArgs args)
    {
        if (sender is MenuItem { CommandParameter: LibraryCardViewModel card })
            await PlayLibraryItemAsync(card);
    }

    private static LibraryCardViewModel? GetContextCard(object? sender) =>
        (sender as MenuItem)?.CommandParameter as LibraryCardViewModel;

    private async void ContextCopyPath_Click(object? sender, RoutedEventArgs args)
    {
        if (GetContextCard(sender) is not { } card) return;
        try
        {
            var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
            if (clipboard is null)
                throw new InvalidOperationException("剪貼簿尚未就緒。");
            await clipboard.SetTextAsync(card.SourcePath);
            _library.ReportError($"已複製來源路徑：{card.Title}");
        }
        catch (Exception exception)
        {
            _library.ReportError($"複製路徑失敗：{exception.Message}");
        }
    }

    private void ContextReveal_Click(object? sender, RoutedEventArgs args)
    {
        if (GetContextCard(sender) is not { } card) return;
        try
        {
            if (!card.IsAvailable)
                throw new FileNotFoundException("來源已離線或不存在。", card.SourcePath);
            var process = new ProcessStartInfo("explorer.exe")
            {
                UseShellExecute = false
            };
            // ArgumentList handles Windows quoting; no shell interpolation.
            process.ArgumentList.Add("/select,");
            process.ArgumentList.Add(card.SourcePath);
            Process.Start(process);
        }
        catch (Exception exception)
        {
            _library.ReportError($"無法開啟檔案總管：{exception.Message}");
        }
    }

    private async void ContextDetails_Click(object? sender, RoutedEventArgs args)
    {
        if (GetContextCard(sender) is not { } card) return;
        var dialog = CreateLibraryDialog(
            card.Title,
            $"類型：{card.KindLabel}\n" +
            $"加入日期：{card.ImportedAtText}\n" +
            $"來源狀態：{card.AvailabilityLabel}\n\n" +
            $"來源位置：\n{card.SourcePath}");
        await dialog.ShowDialog(this);
    }

    private async void ContextRemove_Click(object? sender, RoutedEventArgs args)
    {
        if (GetContextCard(sender) is not { } card) return;
        var dialog = CreateLibraryDialog(
            "從收藏庫移除",
            $"確定從 Lapis 收藏庫移除「{card.Title}」？\n\n" +
            "只會刪除收藏紀錄，不會刪除或更動硬碟上的影片或 BDMV 資料夾。",
            confirmRemoval: true);
        if (await dialog.ShowDialog<bool>(this))
        {
            try { await _library.RemoveAsync(card.Item); }
            catch (Exception exception)
            {
                _library.ReportError($"無法移除收藏：{exception.Message}");
            }
        }
    }

    private static Window CreateLibraryDialog(
        string title, string content, bool confirmRemoval = false)
    {
        var dialog = new Window
        {
            Title = title,
            Width = 480,
            MinHeight = 210,
            SizeToContent = SizeToContent.Height,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            CanResize = false,
            RequestedThemeVariant = ThemeVariant.Light,
            Background = global::Avalonia.Media.Brushes.White
        };
        var cancel = new Button
        {
            Content = confirmRemoval ? "取消" : "關閉",
            HorizontalAlignment = HorizontalAlignment.Right,
            Padding = new global::Avalonia.Thickness(16, 8)
        };
        cancel.Click += (_, _) => dialog.Close(false);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 8
        };
        buttons.Children.Add(cancel);
        if (confirmRemoval)
        {
            var remove = new Button
            {
                Content = "移出收藏庫",
                Background = global::Avalonia.Media.Brushes.Firebrick,
                Foreground = global::Avalonia.Media.Brushes.White,
                Padding = new global::Avalonia.Thickness(16, 8)
            };
            remove.Click += (_, _) => dialog.Close(true);
            buttons.Children.Add(remove);
        }
        dialog.Content = new StackPanel
        {
            Margin = new global::Avalonia.Thickness(24),
            Spacing = 18,
            Children =
            {
                new TextBlock
                {
                    Text = content,
                    TextWrapping = global::Avalonia.Media.TextWrapping.Wrap,
                    FontSize = 13,
                    Foreground = global::Avalonia.Media.Brushes.Black
                },
                buttons
            }
        };
        return dialog;
    }

    private async Task PlayLibraryItemAsync(LibraryCardViewModel card)
    {
        var item = card.Item;
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
            _library.Dispose();
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
