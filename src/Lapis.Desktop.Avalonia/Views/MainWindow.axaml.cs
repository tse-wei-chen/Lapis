using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Platform.Storage;
using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.Controls;
using Lapis.Desktop.Avalonia.Services;
using Lapis.Desktop.Avalonia.ViewModels;

namespace Lapis.Desktop.Avalonia.Views;

public sealed partial class MainWindow : Window
{
    private readonly WindowsVideoSurface _videoSurface;
    private readonly PlayerViewModel _player;
    private bool _closeStarted;
    private bool _shutdownComplete;

    public MainWindow()
    {
        InitializeComponent();
        _videoSurface = this.FindControl<WindowsVideoSurface>("VideoSurface")
            ?? throw new InvalidOperationException("Missing D3D11 surface host.");
        _player = new PlayerViewModel(
            new NativePlayerPlaybackService(() => _videoSurface.OutputHandle));
        DataContext = _player;
        Closing += OnClosing;
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private async void OpenFile_Click(object? sender, RoutedEventArgs args)
    {
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(
                new FilePickerOpenOptions
                {
                    Title = "開啟本機影片",
                    AllowMultiple = false
                });
            if (files.Count == 0) return;
            var path = files[0].TryGetLocalPath();
            if (string.IsNullOrWhiteSpace(path))
            {
                _player.ReportError("目前僅支援本機檔案，不能直接開啟遠端 URI。");
                return;
            }
            await _player.OpenAsync(new MediaLocator(MediaLocatorKind.File, path));
        }
        catch (Exception exception)
        {
            _player.ReportError($"開啟檔案失敗：{exception.Message}");
        }
    }

    private async void OpenBdmv_Click(object? sender, RoutedEventArgs args)
    {
        try
        {
            var folders = await StorageProvider.OpenFolderPickerAsync(
                new FolderPickerOpenOptions
                {
                    Title = "選擇未加密 Blu-ray 根目錄或 BDMV 資料夾",
                    AllowMultiple = false
                });
            if (folders.Count == 0) return;
            var path = folders[0].TryGetLocalPath();
            if (!BdmvLocator.TryFromDirectory(path, out var locator))
            {
                _player.ReportError(
                    "找不到 BDMV/index.bdmv、PLAYLIST 或 STREAM。請選擇有效的資料夾。");
                return;
            }
            // Directory playback requires opt-in libbluray Native DLL, with
            // proper dependency packaging. No menus/BD-J/DRM are claimed.
            await _player.OpenAsync(locator!);
        }
        catch (Exception exception)
        {
            _player.ReportError($"開啟 BDMV 失敗：{exception.Message}");
        }
    }

    private async void OnClosing(object? sender, WindowClosingEventArgs args)
    {
        if (_shutdownComplete) return;
        args.Cancel = true;
        if (_closeStarted) return;
        _closeStarted = true;
        try
        {
            // Native thread join/render-surface detach must finish BEFORE the
            // HWND is destroyed by Avalonia. No synchronous render ACK waits
            // are performed on Avalonia's UI dispatcher.
            await _player.DisposeAsync();
            _videoSurface.ReleaseAfterPlaybackStopped();
            _shutdownComplete = true;
            Close();
        }
        catch (Exception exception)
        {
            _player.ReportError($"無法安全關閉播放視窗：{exception.Message}");
            _closeStarted = false;
        }
    }
}
