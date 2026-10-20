using System.ComponentModel;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using Lapis.Desktop.Avalonia.Services;
using Lapis.Desktop.Avalonia.ViewModels;

namespace Lapis.Desktop.Avalonia.Views;

/// <summary>
/// Owned transparent top-level HWND ABOVE the video child HWND. Avalonia
/// controls must never be placed in the same visual layer as NativeControlHost
/// (the Win32 airspace rule). Closing this view does NOT destroy the GPU HWND.
/// </summary>
public sealed partial class WatchChromeWindow : Window
{
    private readonly Window _owner;
    private readonly PlayerViewModel _player;
    private readonly Func<Task> _back;
    private readonly Action _fullscreenToggle;
    private readonly DispatcherTimer _clock;
    private readonly Border _header;
    private readonly Border _controls;
    private readonly Border _sidebar;
    private readonly Border _subtitleHost;
    private readonly TextBlock _subtitleText;
    private readonly TextBlock _subtitleShadow;
    private readonly Slider _timeline;
    private readonly Button _discHeader;
    private readonly Button _discBottom;
    private readonly ItemsControl _playlistIndex;
    private SidecarSubtitleTrack _subtitleTrack = SidecarSubtitleTrack.Empty;
    private bool _sidebarOpen;
    private bool _chromeVisible = true;
    private bool _disposed;
    private bool _haveCursor;
    private CursorPoint _cursor;
    private DateTime _lastActivity = DateTime.UtcNow;

    // Avalonia runtime XAML loading also requires a public empty ctor.
    public WatchChromeWindow()
    {
        _owner = null!;
        _player = null!;
        _back = null!;
        _fullscreenToggle = null!;
        AvaloniaXamlLoader.Load(this);
        _header = this.FindControl<Border>("WatchHeader")!;
        _controls = this.FindControl<Border>("WatchControls")!;
        _sidebar = this.FindControl<Border>("WatchDiscSidebar")!;
        _subtitleHost = this.FindControl<Border>("WatchSubtitleHost")!;
        _subtitleText = this.FindControl<TextBlock>("WatchSubtitleText")!;
        _subtitleShadow = this.FindControl<TextBlock>("WatchSubtitleShadow")!;
        _timeline = this.FindControl<Slider>("WatchTimeline")!;
        _discHeader = this.FindControl<Button>("WatchDiscToggle")!;
        _discBottom = this.FindControl<Button>("WatchDiscBottom")!;
        _playlistIndex = this.FindControl<ItemsControl>("WatchDiscPlaylistList")!;
        _clock = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(120) };
    }

    public WatchChromeWindow(Window owner, PlayerViewModel player,
        IReadOnlyList<string> playlists, Func<Task> back, Action fullscreenToggle)
        : this()
    {
        _owner = owner;
        _player = player;
        _back = back;
        _fullscreenToggle = fullscreenToggle;
        DataContext = player;
        _playlistIndex.ItemsSource = playlists;
        _discHeader.IsVisible = playlists.Count != 0;
        _discBottom.IsVisible = playlists.Count != 0;
        _timeline.AddHandler(InputElement.PointerPressedEvent,
            OnScrubStart, RoutingStrategies.Tunnel, handledEventsToo: true);
        _timeline.AddHandler(InputElement.PointerReleasedEvent,
            OnScrubEnd, RoutingStrategies.Tunnel, handledEventsToo: true);
        _timeline.AddHandler(InputElement.PointerCaptureLostEvent,
            OnScrubCancelled, RoutingStrategies.Bubble, handledEventsToo: true);
        _player.PropertyChanged += PlayerChanged;
        KeyDown += OnOverlayKeyDown;
        _clock.Tick += ClockTick;
        _clock.Start();
        Closed += (_, _) => DisposeOverlay();
        SyncBounds();
        Show(owner);
    }

    public void SetSubtitleTrack(SidecarSubtitleTrack track)
    {
        _subtitleTrack = track;
        UpdateSubtitle();
    }

    public void NotifyPointerActivity()
    {
        _lastActivity = DateTime.UtcNow;
        SetChromeVisible(true);
    }

    private void WatchPointerMoved(object? sender, PointerEventArgs args) =>
        NotifyPointerActivity();

    private void ClockTick(object? sender, EventArgs args)
    {
        if (_disposed || !_owner.IsVisible) return;
        SyncBounds();

        // Video is a *different HWND* and swallows Avalonia pointer events.
        // Detect mouse motion through Win32 to reveal chrome above the GPU.
        if (OperatingSystem.IsWindows() && GetCursorPos(out var point))
        {
            if (!_haveCursor || point.X != _cursor.X || point.Y != _cursor.Y)
            {
                _cursor = point;
                _haveCursor = true;
                if (point.X >= Position.X && point.Y >= Position.Y &&
                    point.X < Position.X + (int)(Width * RenderScaling) &&
                    point.Y < Position.Y + (int)(Height * RenderScaling))
                    NotifyPointerActivity();
            }
        }

        if (_player.TransportText != "Playing" || _sidebarOpen)
            SetChromeVisible(true);
        else if ((DateTime.UtcNow - _lastActivity).TotalMilliseconds >= 3000)
            SetChromeVisible(false);
    }

    private void SyncBounds()
    {
        if (_disposed) return;
        if (Position != _owner.Position) Position = _owner.Position;
        var size = _owner.ClientSize;
        if (size.Width > 0 && Math.Abs(Width - size.Width) > 1) Width = size.Width;
        if (size.Height > 0 && Math.Abs(Height - size.Height) > 1) Height = size.Height;
    }

    private void SetChromeVisible(bool visible)
    {
        if (_chromeVisible == visible) return;
        _chromeVisible = visible;
        _header.IsVisible = visible;
        _controls.IsVisible = visible;
        _sidebar.IsVisible = visible && _sidebarOpen;
        _subtitleHost.Margin = new Thickness(68, 0, 68, visible ? 146 : 44);
        if (visible)
        {
            LibraryMotion.Enter(_header);
            LibraryMotion.Enter(_controls);
        }
    }

    private void PlayerChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(PlayerViewModel.SeekPositionSeconds))
            UpdateSubtitle();
        else if (e.PropertyName == nameof(PlayerViewModel.TransportText) &&
                 _player.TransportText != "Playing")
            NotifyPointerActivity();
    }

    private void UpdateSubtitle()
    {
        var cue = _subtitleTrack.At(TimeSpan.FromSeconds(_player.SeekPositionSeconds));
        _subtitleText.Text = cue ?? "";
        _subtitleShadow.Text = cue ?? "";
        _subtitleHost.IsVisible = !string.IsNullOrWhiteSpace(cue);
    }

    private async void OnOverlayKeyDown(object? sender, KeyEventArgs args)
    {
        switch (args.Key)
        {
            case Key.Escape:
                args.Handled = true;
                await _back();
                return;
            case Key.F11:
            case Key.F:
                args.Handled = true;
                _fullscreenToggle();
                break;
            case Key.K:
            case Key.Space when args.Source is not Button:
                args.Handled = true;
                if (_player.ToggleCommand.CanExecute(null))
                    _player.ToggleCommand.Execute(null);
                break;
            case Key.J:
            case Key.Left:
                args.Handled = true;
                if (_player.BackCommand.CanExecute(null))
                    _player.BackCommand.Execute(null);
                break;
            case Key.L:
            case Key.Right:
                args.Handled = true;
                if (_player.ForwardCommand.CanExecute(null))
                    _player.ForwardCommand.Execute(null);
                break;
        }
        NotifyPointerActivity();
    }

    private void Disc_Click(object? sender, RoutedEventArgs args) =>
        ToggleDiscNavigation();

    public void ToggleDiscNavigation()
    {
        if (_disposed || !_discBottom.IsVisible) return;
        _sidebarOpen = !_sidebarOpen;
        NotifyPointerActivity();
        _sidebar.IsVisible = _sidebarOpen;
        if (_sidebarOpen) LibraryMotion.Enter(_sidebar);
    }

    private void Fullscreen_Click(object? sender, RoutedEventArgs args)
    {
        _fullscreenToggle();
        NotifyPointerActivity();
    }

    private async void Back_Click(object? sender, RoutedEventArgs args) =>
        await _back();

    private void OnScrubStart(object? sender, PointerPressedEventArgs args)
    {
        if (args.GetCurrentPoint(_timeline).Properties.IsLeftButtonPressed)
        {
            _player.BeginScrub();
            NotifyPointerActivity();
        }
    }

    private async void OnScrubEnd(object? sender, PointerReleasedEventArgs args)
    {
        await _player.CommitScrubAsync();
        NotifyPointerActivity();
    }

    private void OnScrubCancelled(object? sender, PointerCaptureLostEventArgs args) =>
        _player.CancelScrub();

    private void DisposeOverlay()
    {
        if (_disposed) return;
        _disposed = true;
        _clock.Stop();
        _clock.Tick -= ClockTick;
        _player.PropertyChanged -= PlayerChanged;
        KeyDown -= OnOverlayKeyDown;
        _player.CancelScrub();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CursorPoint
    {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out CursorPoint point);
}
