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
using Lapis.Contracts.Playback;
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
    private WatchChromeWindow? _watchChrome;
    private readonly TextSubtitleSourceCatalog _subtitleSources = TextSubtitleSourceCatalog.Default;
    // Invalidated when the Watch screen closes or a new request supersedes it.
    // Sidecar I/O is asynchronous and must not publish into a later session.
    private long _watchGeneration;
    private CancellationTokenSource? _subtitleLoadCancellation;
    private WindowState _windowStateBeforeWatch = WindowState.Normal;
    private readonly Grid _dropOverlay;
    private bool _dropBusy;
    private readonly PlayerViewModel _player;
    private readonly LibraryViewModel _library;
    private readonly JsonWatchProgressStore _watchProgressStore;
    private readonly JsonFavoriteStore _favoriteStore;
    private readonly HashSet<Guid> _pendingFavoriteChanges = [];
    private readonly HashSet<Guid> _pendingLibraryRemovals = [];
    private LibraryCardViewModel? _activeWatchCard;
    private readonly DispatcherTimer _watchProgressTimer = new()
        { Interval = TimeSpan.FromSeconds(10) };
    private bool _closeStarted;
    private bool _shutdownComplete;

    // The UI consumes semantic actions, never XInput structs or device IDs.
    private readonly IGamepadInputSource _gamepadSource = new WindowsXInputSource();
    private readonly GamepadInputRepeater _gamepadRepeater = new();
    private readonly Stopwatch _gamepadClock = Stopwatch.StartNew();
    private readonly DispatcherTimer _gamepadTimer = new()
        { Interval = TimeSpan.FromMilliseconds(33) };
    private bool _controllerBackInFlight;

    public MainWindow()
    {
        InitializeComponent();
        _libraryPage = this.FindControl<Grid>("LibraryPage")
            ?? throw new InvalidOperationException("Missing library page.");
        _playerPage = this.FindControl<Grid>("PlayerPage")
            ?? throw new InvalidOperationException("Missing watch page.");
        _bigPicture = this.FindControl<BigPictureLibraryView>("BigPictureLibrary")
            ?? throw new InvalidOperationException("Missing Big Picture library.");
        _videoSurface = this.FindControl<WindowsVideoSurface>("VideoSurface")
            ?? throw new InvalidOperationException("Missing D3D11 surface host.");
        _dropOverlay = this.FindControl<Grid>("LibraryDropOverlay")
            ?? throw new InvalidOperationException("Missing library drop overlay.");
        var libraryFile = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Lapis", "Library", "catalog.v1.json");
        // Black-box Windows E2E launches the shipped .exe with an isolated
        // catalog. The override is ignored unless explicitly in E2E mode;
        // normal users always keep their own real AppData catalog.
        var e2eCatalog = Environment.GetEnvironmentVariable("LAPIS_E2E_CATALOG");
        if (Environment.GetEnvironmentVariable("LAPIS_E2E_MODE") == "1" &&
            !string.IsNullOrWhiteSpace(e2eCatalog) &&
            Path.IsPathFullyQualified(e2eCatalog))
            libraryFile = e2eCatalog;
        _library = new LibraryViewModel(new JsonMediaLibrary(libraryFile));
        // Same parent path means E2E fixtures get a private bookmark journal.
        _watchProgressStore = new JsonWatchProgressStore(libraryFile + ".watch.v2.json");
        _favoriteStore = new JsonFavoriteStore(libraryFile + ".favorites.v1.json");
        _player = new PlayerViewModel(
            new NativePlayerPlaybackService(() => _videoSurface.OutputHandle));
        _libraryPage.DataContext = _library;
        _playerPage.DataContext = _player;

        _bigPicture.PlayRequested += card => _ = PlayLibraryItemAsync(card, resume: false);
        _bigPicture.ResumeRequested += card => _ = PlayLibraryItemAsync(card, resume: true);
        _bigPicture.ForgetHistoryRequested += card => _ = ForgetHistoryAsync(card);
        _bigPicture.ToggleFavoriteRequested += card => _ = ToggleFavoriteAsync(card);
        _bigPicture.RepairSourceRequested += card => _ = RepairSourceAsync(card);
        _bigPicture.RenameTitleRequested += (card, title) => _ = RenameMovieAsync(card, title);
        _bigPicture.RemoveFromLibraryRequested += card => _ = RemoveMovieAsync(card);
        _bigPicture.ImportRequested += () => _ = ImportFromBigPictureAsync();
        _bigPicture.ImportDiscRequested += () => _ = ImportDiscCoreAsync();

        // No player source, Native DLL or video pipeline is initialized at startup.
        _libraryPage.IsVisible = true;
        _playerPage.IsVisible = false;
        Opened += async (_, _) =>
        {
            try
            {
                await _library.LoadAsync();
                try
                {
                    _library.LoadWatchProgress(await _watchProgressStore.LoadAsync());
                    _library.LoadWatchHistory(await _watchProgressStore.LoadHistoryAsync());
                }
                catch (Exception exception)
                {
                    _library.ReportError($"觀影紀錄無法載入：{exception.Message}");
                }
                try
                {
                    _library.LoadFavorites(await _favoriteStore.LoadAsync());
                }
                catch (Exception exception)
                {
                    _library.ReportError($"我的最愛無法載入：{exception.Message}");
                }
                WindowState = WindowState.Maximized;
                Dispatcher.UIThread.Post(_bigPicture.RestoreMovieFocus,
                    DispatcherPriority.Loaded);
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
        _gamepadTimer.Tick += GamepadTick;
        _watchProgressTimer.Tick += WatchProgressTick;
        Opened += (_, _) => _gamepadTimer.Start();
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

    private void GamepadTick(object? sender, EventArgs args)
    {
        // Never activate the player or move hidden tiles because a gamepad
        // remains attached while the application is in the background.
        if (_closeStarted || (!IsActive && _watchChrome?.IsActive != true))
        {
            _gamepadRepeater.Reset();
            return;
        }

        var actions = _gamepadRepeater.Update(
            _gamepadSource.Read(), _gamepadClock.Elapsed);
        foreach (var action in actions)
        {
            if (_libraryPage.IsVisible)
            {
                _bigPicture.HandleGamepadAction(action);
                continue;
            }

            if (!_playerPage.IsVisible) continue;
            _watchChrome?.NotifyPointerActivity();
            switch (action)
            {
                case GamepadAction.Back:
                    if (!_controllerBackInFlight)
                        _ = ControllerBackToLibraryAsync();
                    break;
                case GamepadAction.Accept:
                    if (_player.ToggleCommand.CanExecute(null))
                        _player.ToggleCommand.Execute(null);
                    break;
                case GamepadAction.Left:
                    if (_player.BackCommand.CanExecute(null))
                        _player.BackCommand.Execute(null);
                    break;
                case GamepadAction.Right:
                    if (_player.ForwardCommand.CanExecute(null))
                        _player.ForwardCommand.Execute(null);
                    break;
                case GamepadAction.Menu:
                    _watchChrome?.ToggleDiscNavigation();
                    break;
            }
        }
    }

    private async Task ControllerBackToLibraryAsync()
    {
        if (_controllerBackInFlight) return;
        _controllerBackInFlight = true;
        try { await BackToLibraryAsync(); }
        catch (Exception ex) { _library.ReportError($"返回收藏庫失敗：{ex.Message}"); }
        finally { _controllerBackInFlight = false; }
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

    private void GoBigPicture_Click(object? sender, RoutedEventArgs args)
    {
        _bigPicture.CloseDetails();
        _library.ShowAll();
        _library.SearchText = "";
        _bigPicture.ResetScroll();
    }

    private void ShellAll_Click(object? sender, RoutedEventArgs args) =>
        ShellNavigate(_library.ShowAll);
    private void ShellRecent_Click(object? sender, RoutedEventArgs args) =>
        ShellNavigate(_library.ShowRecentlyAdded);
    private void ShellBluRay_Click(object? sender, RoutedEventArgs args) =>
        ShellNavigate(_library.ShowDiscs);
    private void ShellVideos_Click(object? sender, RoutedEventArgs args) =>
        ShellNavigate(_library.ShowVideos);
    private void ShellHistory_Click(object? sender, RoutedEventArgs args) =>
        ShellNavigate(_library.ShowHistory);
    private void ShellFavorites_Click(object? sender, RoutedEventArgs args) =>
        ShellNavigate(_library.ShowFavorites);

    private void ShellNavigate(Action navigate)
    {
        _bigPicture.CloseDetails();
        navigate();
        _bigPicture.ResetScroll();
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

    private async Task PlayLibraryItemAsync(LibraryCardViewModel card, bool resume = false)
    {
        if (_closeStarted || !_libraryPage.IsVisible || _playerPage.IsVisible) return;
        var watchGeneration = ++_watchGeneration;
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
            if (watchGeneration != _watchGeneration) return;
            await _player.OpenAsync(source);
            if (watchGeneration != _watchGeneration) return;
            if (!_player.CanTransport)
            {
                _library.ReportError(_player.StatusText);
                RestoreLibraryView();
                return;
            }

            // Resuming BDMV navigation is intentionally unsupported until the
            // native title/chapter seek contract is available.
            if (resume && !item.IsDisc && card.CanResume)
                await _player.ResumeAtAsync(card.ResumePositionSeconds);
            if (watchGeneration != _watchGeneration) return;
            _activeWatchCard = card;
            _watchProgressTimer.Start();

            var playlists = item.IsDisc
                ? DiscPlaylistIndex.ListIdentifiers(item)
                    .Select(id => id + ".mpls").ToArray()
                : [];
            // Build a per-Watch registry: local SRT remains Auto, WebVTT
            // stays selectable, and only the active file playback session
            // may expose supported embedded text streams. No PGS text entry.
            var watchSources = source.Kind == MediaLocatorKind.File &&
                               _player.EmbeddedSubtitlePlayback is { } embedded
                ? new TextSubtitleSourceCatalog(
                [
                    new SidecarSrtTextSubtitleSource(),
                    new SidecarWebVttTextSubtitleSource(),
                    new EmbeddedTextSubtitleSource(embedded, source.Value)
                ])
                : _subtitleSources;
            var chrome = new WatchChromeWindow(this, _player, playlists,
                BackToLibraryAsync, ToggleWatchFullscreen, watchSources);
            _watchChrome = chrome;
            if (source.Kind == MediaLocatorKind.File)
            {
                var cancellation = new CancellationTokenSource();
                _subtitleLoadCancellation = cancellation;
                var track = await _subtitleSources.LoadPreferredAsync(
                    source.Value, System.Globalization.CultureInfo.CurrentUICulture,
                    cancellation.Token);
                // A slow subtitle read may finish after Back or a newer Watch.
                if (watchGeneration == _watchGeneration &&
                    ReferenceEquals(_watchChrome, chrome))
                {
                    chrome.SetSubtitleTrack(track);
                    await chrome.ConfigureSubtitleChoicesAsync(source.Value,
                        cancellation.Token);
                }
            }
        }
        catch (OperationCanceledException) when (watchGeneration != _watchGeneration)
        {
            // Normal: Watch closed while the asynchronous sidecar was loading.
        }
        catch (Exception exception)
        {
            if (watchGeneration != _watchGeneration) return;
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
        _watchProgressTimer.Stop();
        _activeWatchCard = null;
        InvalidateWatchSubtitleLoad();
        _watchChrome?.Close();
        _watchChrome = null;
        WindowState = _windowStateBeforeWatch;
        _playerPage.IsVisible = false;
        _libraryPage.IsVisible = true;
        Dispatcher.UIThread.Post(() =>
        {
            _bigPicture.RestoreMovieFocus();
        }, DispatcherPriority.Loaded);
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
        _watchProgressTimer.Stop();
        await SaveWatchProgressAsync();
        // Cancel background subtitle reads immediately, before native shutdown.
        InvalidateWatchSubtitleLoad();
        // Native render worker is stopped/detached before hiding the host.
        await _player.CloseAsync();
        RestoreLibraryView();
    }

    private async Task RepairSourceAsync(LibraryCardViewModel card)
    {
        if (_closeStarted) return;
        try
        {
            if (card.IsAvailable)
            {
                card.RefreshSourceAvailability();
                _library.ReportError("來源已重新連接，無需更換路徑。");
                return;
            }

            MediaLocator replacement;
            if (card.IsDisc)
            {
                var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
                {
                    Title = "選擇原本 Blu-ray 光碟的新根目錄（包含 BDMV）",
                    AllowMultiple = false
                });
                var folder = folders.Count > 0 ? folders[0].TryGetLocalPath() : null;
                if (string.IsNullOrWhiteSpace(folder)) return;
                replacement = new MediaLocator(MediaLocatorKind.Directory, folder);
            }
            else
            {
                var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
                {
                    Title = "選擇原本同一部影片的新檔案位置",
                    AllowMultiple = false
                });
                var file = files.Count > 0 ? files[0].TryGetLocalPath() : null;
                if (string.IsNullOrWhiteSpace(file)) return;
                replacement = new MediaLocator(MediaLocatorKind.File, file);
            }

            if (_closeStarted) return;
            await _library.RelinkAsync(card.Item, replacement);
            _bigPicture.RestoreMovieFocus();
        }
        catch (Exception exception)
        {
            if (!_closeStarted)
                _library.ReportError($"修復來源失敗：{exception.Message}");
        }
    }

    /// <summary>
    /// One removal path for the unified library detail sheet.
    /// The catalog is the source of truth: never remove the physical media.
    /// After a successful catalog write, remove orphaned bookmark/favorite
    /// sidecars and update the corresponding in-memory shelves.
    /// </summary>
    private async Task RemoveMovieAsync(LibraryCardViewModel card)
    {
        if (_closeStarted || !_pendingLibraryRemovals.Add(card.Item.Id)) return;
        try
        {
            await _library.RemoveAsync(card.Item);
            var cleanupErrors = new List<string>();
            try
            {
                await _favoriteStore.SetAsync(card.Item.Id, false);
                if (!_closeStarted) _library.SetFavorite(card.Item.Id, false);
            }
            catch (Exception exception)
            {
                cleanupErrors.Add($"最愛：{exception.Message}");
            }

            try
            {
                await _watchProgressStore.ForgetAsync(card.Item.Id);
                if (!_closeStarted) _library.ForgetWatchHistory(card.Item.Id);
            }
            catch (Exception exception)
            {
                cleanupErrors.Add($"觀看紀錄：{exception.Message}");
            }

            if (!_closeStarted)
            {
                if (cleanupErrors.Count > 0)
                    _library.ReportError("收藏已移除，但部分關聯紀錄清理失敗：" +
                        string.Join("；", cleanupErrors));
                else
                    Dispatcher.UIThread.Post(_bigPicture.RestoreMovieFocus,
                        DispatcherPriority.Loaded);
            }
        }
        catch (Exception exception)
        {
            if (!_closeStarted)
                _library.ReportError($"無法移除收藏：{exception.Message}");
        }
        finally { _pendingLibraryRemovals.Remove(card.Item.Id); }
    }

    private async Task RenameMovieAsync(LibraryCardViewModel card, string title)
    {
        if (_closeStarted) return;
        try
        {
            await _library.RenameAsync(card.Item, title);
            if (!_closeStarted)
                Dispatcher.UIThread.Post(_bigPicture.RestoreMovieFocus,
                    DispatcherPriority.Loaded);
        }
        catch (Exception exception)
        {
            if (!_closeStarted)
                _library.ReportError($"無法修改電影名稱：{exception.Message}");
        }
    }

    private async Task ToggleFavoriteAsync(LibraryCardViewModel card)
    {
        var id = card.Item.Id;
        if (!_pendingFavoriteChanges.Add(id)) return;
        try
        {
            var next = !card.IsFavorite;
            await _favoriteStore.SetAsync(id, next);
            if (!_closeStarted)
            {
                _library.SetFavorite(id, next);
                _library.ReportError(next
                    ? $"已加入我的最愛：{card.Title}" : $"已移出我的最愛：{card.Title}");
            }
        }
        catch (Exception exception)
        {
            if (!_closeStarted)
                _library.ReportError($"無法更新我的最愛：{exception.Message}");
        }
        finally
        {
            _pendingFavoriteChanges.Remove(id);
        }
    }

    private async Task ForgetHistoryAsync(LibraryCardViewModel card)
    {
        try
        {
            await _watchProgressStore.ForgetAsync(card.Item.Id);
            _library.ForgetWatchHistory(card.Item.Id);
        }
        catch (Exception exception)
        {
            _library.ReportError($"無法移除觀看紀錄：{exception.Message}");
        }
    }

    private async void WatchProgressTick(object? sender, EventArgs args) =>
        await SaveWatchProgressAsync();

    private async Task SaveWatchProgressAsync()
    {
        var card = _activeWatchCard;
        if (card is null || card.IsDisc) return;
        var snapshot = _player.PlaybackSnapshot;
        if (snapshot.Transport is not (TransportState.Ready or
            TransportState.Playing or TransportState.Paused or TransportState.Ended))
            return;
        var duration = snapshot.Duration?.ToTimeSpan().TotalSeconds ?? 0;
        if (!double.IsFinite(duration) || duration < 60) return;
        var seconds = snapshot.Transport == TransportState.Ended
            ? duration : snapshot.Position.ToTimeSpan().TotalSeconds;
        if (!double.IsFinite(seconds)) return;
        var progress = new WatchProgress(card.Item.Id, seconds, duration,
            DateTimeOffset.UtcNow);
        try
        {
            // The timer and the final close save can overlap. Apply only
            // observations accepted by the journal's monotonic time guard.
            if (await _watchProgressStore.SaveAsync(progress) && !_closeStarted)
                _library.ApplyWatchSnapshot(progress);
        }
        catch (Exception exception)
        {
            System.Diagnostics.Trace.TraceWarning(
                "Failed to save watch progress: {0}", exception);
            if (!_closeStarted) _library.ReportError($"觀影進度無法儲存：{exception.Message}");
        }
    }

    private void InvalidateWatchSubtitleLoad()
    {
        ++_watchGeneration;
        var cancellation = _subtitleLoadCancellation;
        _subtitleLoadCancellation = null;
        if (cancellation is null) return;
        cancellation.Cancel();
        cancellation.Dispose();
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
            _gamepadTimer.Stop();
            _gamepadTimer.Tick -= GamepadTick;
            _watchProgressTimer.Stop();
            _watchProgressTimer.Tick -= WatchProgressTick;
            await SaveWatchProgressAsync();
            InvalidateWatchSubtitleLoad();
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
