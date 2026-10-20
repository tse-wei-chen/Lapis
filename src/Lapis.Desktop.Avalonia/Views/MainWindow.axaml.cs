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
    private readonly Grid _libraryPage;
    private readonly Grid _playerPage;
    private readonly BigPictureLibraryView _bigPicture;
    private readonly Border _legacyLibraryHeader;
    private readonly Grid _legacyLibraryBody;
    private WatchChromeWindow? _watchChrome;
    private WindowState _windowStateBeforeWatch = WindowState.Normal;
    private readonly ScrollViewer _gridSurface;
    private readonly Border _listSurface;
    private readonly Grid _dropOverlay;
    private bool _dropBusy;
    private readonly Grid _navigationHost;
    private readonly Border _navigationIndicator;
    private readonly Button _navAll;
    private readonly Button _navBluRay;
    private readonly Button _navVideos;
    private readonly Button _navRecent;
    private readonly TextBlock _currentHeading;
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
        _bigPicture = this.FindControl<BigPictureLibraryView>("BigPictureLibrary")
            ?? throw new InvalidOperationException("Missing Big Picture library.");
        _legacyLibraryHeader = this.FindControl<Border>("LegacyLibraryHeader")
            ?? throw new InvalidOperationException("Missing library management header.");
        _legacyLibraryBody = this.FindControl<Grid>("LegacyLibraryBody")
            ?? throw new InvalidOperationException("Missing library management body.");
        _videoSurface = this.FindControl<WindowsVideoSurface>("VideoSurface")
            ?? throw new InvalidOperationException("Missing D3D11 surface host.");
        _gridSurface = this.FindControl<ScrollViewer>("LibraryGridSurface")
            ?? throw new InvalidOperationException("Missing library grid viewport.");
        _listSurface = this.FindControl<Border>("LibraryListSurface")
            ?? throw new InvalidOperationException("Missing library list viewport.");
        _dropOverlay = this.FindControl<Grid>("LibraryDropOverlay")
            ?? throw new InvalidOperationException("Missing library drop overlay.");
        _navigationHost = this.FindControl<Grid>("LibraryNavigationHost")
            ?? throw new InvalidOperationException("Missing sidebar host.");
        _navigationIndicator = this.FindControl<Border>("LibraryNavigationIndicator")
            ?? throw new InvalidOperationException("Missing sidebar selection indicator.");
        _navAll = this.FindControl<Button>("NavigationAll")
            ?? throw new InvalidOperationException("Missing All nav item.");
        _navBluRay = this.FindControl<Button>("NavigationBluRay")
            ?? throw new InvalidOperationException("Missing Blu-ray nav item.");
        _navVideos = this.FindControl<Button>("NavigationVideos")
            ?? throw new InvalidOperationException("Missing Videos nav item.");
        _navRecent = this.FindControl<Button>("NavigationRecent")
            ?? throw new InvalidOperationException("Missing Recently Added nav item.");
        _currentHeading = this.FindControl<TextBlock>("LibraryCurrentHeading")
            ?? throw new InvalidOperationException("Missing library heading.");

        var libraryFile = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Lapis", "Library", "catalog.v1.json");
        _library = new LibraryViewModel(new JsonMediaLibrary(libraryFile));
        _player = new PlayerViewModel(
            new NativePlayerPlaybackService(() => _videoSurface.OutputHandle));
        _libraryPage.DataContext = _library;
        _playerPage.DataContext = _player;

        _bigPicture.PlayRequested += card => _ = PlayLibraryItemAsync(card);
        _bigPicture.ImportRequested += () => _ = ImportFromBigPictureAsync();
        _bigPicture.ImportDiscRequested += () => _ = ImportDiscCoreAsync();
        _bigPicture.ManageRequested += () => ShowLibraryManagement(true);

        // No player source, Native DLL or video pipeline is initialized at startup.
        _libraryPage.IsVisible = true;
        _playerPage.IsVisible = false;
        ShowLibraryManagement(false);
        Opened += async (_, _) =>
        {
            try
            {
                await _library.LoadAsync();
                WindowState = WindowState.Maximized;
                // The old management sidebar is kept as a deliberate secondary
                // screen; it is not the default startup experience.
            }
            catch (Exception exception) { _library.ReportError($"無法讀取收藏庫：{exception.Message}"); }
        };

        // OS Explorer drops are handled by the library screen; Watch never
        // accepts dropped content and no import initiates Native playback.
        _libraryPage.AddHandler(DragDrop.DragEnterEvent,
            LibraryDragEnter, RoutingStrategies.Bubble, handledEventsToo: true);
        _libraryPage.AddHandler(DragDrop.DragOverEvent,
            LibraryDragOver, RoutingStrategies.Bubble, handledEventsToo: true);
        _libraryPage.AddHandler(DragDrop.DragLeaveEvent,
            LibraryDragLeave, RoutingStrategies.Bubble, handledEventsToo: true);
        _libraryPage.AddHandler(DragDrop.DropEvent,
            LibraryDrop, RoutingStrategies.Bubble, handledEventsToo: true);

        KeyDown += WatchKeyDown;
        Closing += OnClosing;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private bool CanAcceptFiles(DragEventArgs args) =>
        !_dropBusy && _libraryPage.IsVisible &&
        args.DataTransfer.Contains(DataFormat.File);

    private void LibraryDragEnter(object? sender, DragEventArgs args) =>
        LibraryDragOver(sender, args);

    private void LibraryDragOver(object? sender, DragEventArgs args)
    {
        var valid = CanAcceptFiles(args);
        args.DragEffects = valid ? DragDropEffects.Copy : DragDropEffects.None;
        args.Handled = true;
        if (valid && !_dropOverlay.IsVisible)
        {
            _dropOverlay.IsVisible = true;
            Dispatcher.UIThread.Post(() => LibraryMotion.Enter(_dropOverlay),
                DispatcherPriority.Loaded);
        }
        else if (!valid)
        {
            _dropOverlay.IsVisible = false;
        }
    }

    private void LibraryDragLeave(object? sender, DragEventArgs args)
    {
        // A leave event can originate when the pointer crosses child controls.
        // Hide only when leaving the whole library surface.
        var position = args.GetPosition(_libraryPage);
        if (position.X < 0 || position.Y < 0 ||
            position.X > _libraryPage.Bounds.Width ||
            position.Y > _libraryPage.Bounds.Height)
        {
            _dropOverlay.IsVisible = false;
        }
    }

    private async void LibraryDrop(object? sender, DragEventArgs args)
    {
        var accept = CanAcceptFiles(args);
        args.DragEffects = accept ? DragDropEffects.Copy : DragDropEffects.None;
        args.Handled = true;
        _dropOverlay.IsVisible = false;
        if (!accept) return;

        // Read the paths before the platform disposes its drop data.
        var paths = args.DataTransfer.TryGetFiles()
            ?.Select(item => item.TryGetLocalPath())
            .ToArray() ?? [];
        if (paths.Length == 0)
        {
            _library.ReportError("沒有可存取的本機影片或 Blu-ray 資料夾。");
            return;
        }

        _dropBusy = true;
        try
        {
            var plan = LibraryDropClassifier.Classify(paths);
            var result = await _library.ImportManyAsync(
                plan.Sources, plan.Rejected.Count);
            if (plan.Rejected.Count != 0)
            {
                _library.ReportError(
                    $"拖曳匯入：新增 {result.Added} 部、已收藏 {result.AlreadyPresent} 部、" +
                    $"略過 {result.Skipped} 個。{plan.Rejected[0]}");
            }
        }
        catch (Exception ex)
        {
            _library.ReportError($"拖曳匯入失敗：{ex.Message}");
        }
        finally
        {
            _dropBusy = false;
            _dropOverlay.IsVisible = false;
        }
    }

    private async void WatchKeyDown(object? sender, KeyEventArgs args)
    {
        if (_libraryPage.IsVisible)
        {
            if (args.Key == Key.F11)
            {
                WindowState = WindowState == WindowState.FullScreen
                    ? WindowState.Maximized : WindowState.FullScreen;
                args.Handled = true;
            }
            else if (args.Key == Key.Escape && !_bigPicture.IsVisible)
            {
                ShowLibraryManagement(false);
                args.Handled = true;
            }
            return;
        }
        if (!_playerPage.IsVisible) return;
        switch (args.Key)
        {
            case Key.Escape:
                args.Handled = true;
                await BackToLibraryAsync();
                break;
            case Key.F11:
            case Key.F:
                args.Handled = true;
                ToggleWatchFullscreen();
                break;
            case Key.Space:
            case Key.K:
                args.Handled = true;
                if (_player.ToggleCommand.CanExecute(null))
                    _player.ToggleCommand.Execute(null);
                break;
            case Key.Left:
            case Key.J:
                args.Handled = true;
                if (_player.BackCommand.CanExecute(null))
                    _player.BackCommand.Execute(null);
                break;
            case Key.Right:
            case Key.L:
                args.Handled = true;
                if (_player.ForwardCommand.CanExecute(null))
                    _player.ForwardCommand.Execute(null);
                break;
        }
        _watchChrome?.NotifyPointerActivity();
    }

    private void ShowLibraryManagement(bool show)
    {
        _bigPicture.IsVisible = !show;
        _legacyLibraryHeader.IsVisible = show;
        _legacyLibraryBody.IsVisible = show;
        if (show)
            Dispatcher.UIThread.Post(() => UpdateNavigationMarker(initial: true),
                DispatcherPriority.Loaded);
        else
            Dispatcher.UIThread.Post(() => LibraryMotion.Enter(_bigPicture),
                DispatcherPriority.Loaded);
    }

    private async Task ImportFromBigPictureAsync()
    {
        // Reuse the same picker and catalog API as the management screen.
        await ImportFilesCoreAsync();
    }

    private async void ImportFiles_Click(object? sender, RoutedEventArgs args) =>
        await ImportFilesCoreAsync();

    private async Task ImportFilesCoreAsync()
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

    private async void ImportDisc_Click(object? sender, RoutedEventArgs args) =>
        await ImportDiscCoreAsync();

    private async Task ImportDiscCoreAsync()
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

    private void ShowAll_Click(object? sender, RoutedEventArgs args) =>
        NavigateLibrary(_library.ShowAll);
    private void ShowDiscs_Click(object? sender, RoutedEventArgs args) =>
        NavigateLibrary(_library.ShowDiscs);
    private void ShowVideos_Click(object? sender, RoutedEventArgs args) =>
        NavigateLibrary(_library.ShowVideos);
    private void ShowRecentlyAdded_Click(object? sender, RoutedEventArgs args) =>
        NavigateLibrary(_library.ShowRecentlyAdded);

    private void NavigateLibrary(Action navigate)
    {
        var previous = _library.Category;
        navigate();
        if (previous == _library.Category) return;
        // The actor does not participate in navigation animations. Update
        // the read-only compositor visuals after Avalonia has laid out the
        // newly selected content.
        Dispatcher.UIThread.Post(() =>
        {
            if (!_libraryPage.IsVisible) return;
            UpdateNavigationMarker(initial: false);
            LibraryMotion.Enter(_library.IsGridView ? _gridSurface : _listSurface);
            LibraryMotion.Enter(_currentHeading);
        }, DispatcherPriority.Loaded);
    }

    private void UpdateNavigationMarker(bool initial)
    {
        if (!_libraryPage.IsVisible) return;
        var selected = _library.Category switch
        {
            LibraryCategory.BluRay => _navBluRay,
            LibraryCategory.Videos => _navVideos,
            LibraryCategory.RecentlyAdded => _navRecent,
            _ => _navAll
        };
        var origin = global::Avalonia.VisualExtensions.TranslatePoint(
            selected, new global::Avalonia.Point(0, 0), _navigationHost);
        if (origin is null || selected.Bounds.Height <= 0) return;
        var y = LibraryMotion.IndicatorTop(origin.Value.Y, selected.Bounds.Height,
            _navigationIndicator.Bounds.Height);
        LibraryMotion.MoveNavigationMarker(_navigationIndicator, y, animate: !initial);
    }
    private void ShowGrid_Click(object? sender, RoutedEventArgs args)
    {
        if (_library.IsGridView) return;
        _library.ShowGrid();
        Dispatcher.UIThread.Post(() => LibraryMotion.Enter(_gridSurface),
            DispatcherPriority.Loaded);
    }

    private void ShowList_Click(object? sender, RoutedEventArgs args)
    {
        if (_library.IsListView) return;
        _library.ShowList();
        Dispatcher.UIThread.Post(() => LibraryMotion.Enter(_listSurface),
            DispatcherPriority.Loaded);
    }

    private void LibraryItem_PointerEntered(object? sender, PointerEventArgs args)
    {
        if (sender is Control item) LibraryMotion.Hover(item, true);
    }

    private void LibraryItem_PointerExited(object? sender, PointerEventArgs args)
    {
        if (sender is Control item) LibraryMotion.Hover(item, false);
    }

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
            var source = LibrarySourcePaths.Normalize(item.ToLocator());
            // Full-screen video is the primary surface. Chrome is an owned,
            // separately composed transparent top-level window, NOT an
            // Avalonia child under the NativeControlHost HWND.
            _windowStateBeforeWatch = WindowState;
            _libraryPage.IsVisible = false;
            _playerPage.IsVisible = true;
            WindowState = WindowState.FullScreen;

            await AwaitVideoSurfaceAsync();
            await _player.OpenAsync(source);
            if (!_player.CanTransport)
            {
                _library.ReportError(_player.StatusText);
                RestoreLibraryView();
                return;
            }

            var playlists = item.IsDisc
                ? DiscPlaylistIndex.ListIdentifiers(item)
                    .Select(id => id + ".mpls").ToArray()
                : [];
            _watchChrome = new WatchChromeWindow(this, _player, playlists,
                BackToLibraryAsync, ToggleWatchFullscreen);
            if (source.Kind == MediaLocatorKind.File)
            {
                var track = await SidecarSubtitleTrack.LoadForFileAsync(source.Value);
                _watchChrome?.SetSubtitleTrack(track);
            }
        }
        catch (Exception exception)
        {
            _library.ReportError($"無法播放收藏項目：{exception.Message}");
            try { await _player.CloseAsync(); }
            catch (Exception closeException)
            {
                _library.ReportError($"無法安全關閉播放：{closeException.Message}");
            }
            RestoreLibraryView();
        }
    }

    private void ToggleWatchFullscreen()
    {
        if (!_playerPage.IsVisible) return;
        WindowState = WindowState == WindowState.FullScreen
            ? WindowState.Maximized : WindowState.FullScreen;
        _watchChrome?.NotifyPointerActivity();
    }

    private void RestoreLibraryView()
    {
        _watchChrome?.Close();
        _watchChrome = null;
        WindowState = _windowStateBeforeWatch;
        _playerPage.IsVisible = false;
        _libraryPage.IsVisible = true;
        Dispatcher.UIThread.Post(() => UpdateNavigationMarker(initial: true),
            DispatcherPriority.Loaded);
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

    private async Task BackToLibraryAsync()
    {
        // Native render worker is stopped/detached before hiding the host.
        await _player.CloseAsync();
        RestoreLibraryView();
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
            _watchChrome?.Close();
            _watchChrome = null;
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
