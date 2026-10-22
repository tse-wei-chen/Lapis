using System.ComponentModel;
using Lapis.Contracts.Media;
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
    private readonly ComboBox _subtitlePicker;
    private string? _subtitleVideoPath;
    private CancellationToken _subtitleLifetimeToken;
    private CancellationTokenSource? _subtitleChoiceCancellation;
    private int _subtitleChoiceGeneration;
    private ulong? _embeddedSelectionEpoch;
    private bool _settingSubtitleChoices;
    private sealed record SubtitleChoice(string Label, TextSubtitleDescriptor? Track, bool Off = false)
    {
        public override string ToString() => Label;
    }
    private readonly Button _discHeader;
    private readonly Button _discBottom;
    private readonly Button _backSeek;
    private readonly Button _forwardSeek;
    private readonly ItemsControl _playlistIndex;
    private readonly TextSubtitleSourceCatalog _subtitleSources;
    private ITextSubtitleTrack _subtitleTrack = SidecarSubtitleTrack.Empty;
    private bool _sidebarOpen;
    private bool _chromeVisible = true;
    private bool _disposed;
    private bool _haveCursor;
    private CursorPoint _cursor;
    private DateTime _lastActivity = DateTime.UtcNow;
    private static readonly TimeSpan SeekHoldThreshold =
        TimeSpan.FromMilliseconds(450);
    private DateTime? _seekButtonPressedAt;
    private bool _backSeekPressed;
    private bool _forwardSeekPressed;
    private bool _fastForwardActive;
    private bool _topmostSyncFailureReported;
    private int _seekPressGeneration;
    private CancellationTokenSource? _fastForwardDelay;
    private Task<bool>? _fastForwardStartTask;

    // Avalonia runtime XAML loading also requires a public empty ctor.
    public WatchChromeWindow()
    {
        _owner = null!;
        _player = null!;
        _back = null!;
        _fullscreenToggle = null!;
        _subtitleSources = TextSubtitleSourceCatalog.Default;
        AvaloniaXamlLoader.Load(this);
        _header = this.FindControl<Border>("WatchHeader")!;
        _controls = this.FindControl<Border>("WatchControls")!;
        _sidebar = this.FindControl<Border>("WatchDiscSidebar")!;
        _subtitleHost = this.FindControl<Border>("WatchSubtitleHost")!;
        _subtitleText = this.FindControl<TextBlock>("WatchSubtitleText")!;
        _subtitleShadow = this.FindControl<TextBlock>("WatchSubtitleShadow")!;
        _timeline = this.FindControl<Slider>("WatchTimeline")!;
        _subtitlePicker = this.FindControl<ComboBox>("WatchSubtitlePicker")!;
        _subtitlePicker.SelectionChanged += SubtitlePickerChanged;
        _discHeader = this.FindControl<Button>("WatchDiscToggle")!;
        _discBottom = this.FindControl<Button>("WatchDiscBottom")!;
        _backSeek = this.FindControl<Button>("WatchBackSeek")!;
        _forwardSeek = this.FindControl<Button>("WatchForwardSeek")!;
        _playlistIndex = this.FindControl<ItemsControl>("WatchDiscPlaylistList")!;
        _clock = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(120) };
    }

    public WatchChromeWindow(Window owner, PlayerViewModel player,
        IReadOnlyList<string> playlists, Func<Task> back, Action fullscreenToggle,
        TextSubtitleSourceCatalog? subtitleSources = null)
        : this()
    {
        _owner = owner;
        _player = player;
        _back = back;
        _fullscreenToggle = fullscreenToggle;
        _subtitleSources = subtitleSources ?? TextSubtitleSourceCatalog.Default;
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
        _backSeek.AddHandler(InputElement.PointerPressedEvent,
            OnBackSeekPressed, RoutingStrategies.Tunnel, handledEventsToo: true);
        _backSeek.AddHandler(InputElement.PointerReleasedEvent,
            OnBackSeekReleased, RoutingStrategies.Tunnel, handledEventsToo: true);
        _backSeek.AddHandler(InputElement.PointerCaptureLostEvent,
            OnSeekButtonCaptureLost, RoutingStrategies.Bubble, handledEventsToo: true);
        _forwardSeek.AddHandler(InputElement.PointerPressedEvent,
            OnForwardSeekPressed, RoutingStrategies.Tunnel, handledEventsToo: true);
        _forwardSeek.AddHandler(InputElement.PointerReleasedEvent,
            OnForwardSeekReleased, RoutingStrategies.Tunnel, handledEventsToo: true);
        _forwardSeek.AddHandler(InputElement.PointerCaptureLostEvent,
            OnSeekButtonCaptureLost, RoutingStrategies.Bubble, handledEventsToo: true);
        _player.PropertyChanged += PlayerChanged;
        KeyDown += OnOverlayKeyDown;
        _clock.Tick += ClockTick;
        _clock.Start();
        Closed += (_, _) => DisposeOverlay();
        if (OperatingSystem.IsWindows())
        {
            _owner.Activated += OwnerActivated;
            _owner.Deactivated += OwnerDeactivated;
            Opened += (_, _) => SetOverlayTopmost(ShouldOverlayBeTopmost());
        }
        SyncBounds();
        Show(owner);
        if (OperatingSystem.IsWindows())
            Dispatcher.UIThread.Post(
                () => SetOverlayTopmost(ShouldOverlayBeTopmost()),
                DispatcherPriority.Background);
    }

    public void SetSubtitleTrack(ITextSubtitleTrack track)
    {
        ArgumentNullException.ThrowIfNull(track);
        if (_disposed)
        {
            if (track is IAsyncDisposable rejected) _ = RetireSubtitleAsync(rejected);
            return;
        }
        var previous = _subtitleTrack;
        _subtitleTrack = track;
        UpdateSubtitle();
        // Off, another text source, or Seek must retire the old native worker.
        // Never await I/O on Avalonia's presentation dispatcher.
        if (!ReferenceEquals(previous, track) && previous is IAsyncDisposable retired)
            _ = RetireSubtitleAsync(retired);
    }

    private static async Task RetireSubtitleAsync(IAsyncDisposable track)
    {
        try { await track.DisposeAsync(); }
        catch (Exception exception)
        {
            System.Diagnostics.Trace.TraceWarning(
                "Lapis embedded subtitle track retirement failed: {0}", exception);
        }
    }

    /// <summary>
    /// Publish subtitle choices only for this Watch session. The initial
    /// "Auto" entry uses the track already loaded by MainWindow, so there is
    /// no second disk read on first display.
    /// </summary>
    public async Task ConfigureSubtitleChoicesAsync(
        string videoPath, CancellationToken cancellationToken)
    {
        var tracks = await _subtitleSources.DiscoverAsync(
            videoPath, cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        if (_disposed) return;

        _subtitleVideoPath = videoPath;
        _subtitleLifetimeToken = cancellationToken;
        var choices = new List<SubtitleChoice>
        {
            new("字幕：自動", null),
            new("字幕：關閉", null, Off: true)
        };
        choices.AddRange(tracks.Select(track =>
            new SubtitleChoice(track.DisplayName, track)));

        _settingSubtitleChoices = true;
        try
        {
            _subtitlePicker.ItemsSource = choices;
            _subtitlePicker.SelectedIndex = 0;
            _subtitlePicker.IsVisible = tracks.Count != 0;
        }
        finally { _settingSubtitleChoices = false; }
    }

    private async void SubtitlePickerChanged(object? sender, SelectionChangedEventArgs args)
    {
        if (_disposed || _settingSubtitleChoices ||
            _subtitlePicker.SelectedItem is not SubtitleChoice choice ||
            _subtitleVideoPath is null)
            return;
        await LoadSubtitleChoiceAsync(choice);
    }

    private async Task LoadSubtitleChoiceAsync(SubtitleChoice choice)
    {
        if (_disposed || _subtitleVideoPath is null) return;

        // The selected descriptor survives Seek; its decoded cues do not.
        // The view clock schedules a fresh load when playback Epoch changes.
        _embeddedSelectionEpoch = choice.Track?.Kind == TextSubtitleSourceKind.EmbeddedText
            ? _player.PlaybackSnapshot.Epoch.Value : null;
        // Every selection starts a new generation. A slow previous read must
        // not publish after the user picked another language or turned it off.
        var generation = ++_subtitleChoiceGeneration;
        _subtitleChoiceCancellation?.Cancel();
        _subtitleChoiceCancellation?.Dispose();
        _subtitleChoiceCancellation = null;
        NotifyPointerActivity();
        SetSubtitleTrack(SidecarSubtitleTrack.Empty);
        if (choice.Off) return;

        var cancellation = CancellationTokenSource.CreateLinkedTokenSource(
            _subtitleLifetimeToken);
        _subtitleChoiceCancellation = cancellation;
        try
        {
            var track = choice.Track is null
                ? await _subtitleSources.LoadPreferredAsync(
                    _subtitleVideoPath, System.Globalization.CultureInfo.CurrentUICulture,
                    cancellation.Token)
                : await _subtitleSources.LoadAsync(
                    _subtitleVideoPath, choice.Track, cancellation.Token);
            if (!_disposed && generation == _subtitleChoiceGeneration &&
                !cancellation.IsCancellationRequested)
                SetSubtitleTrack(track);
            else if (track is IAsyncDisposable discarded)
                await RetireSubtitleAsync(discarded);
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            // Obsolete selection or the Watch session has closed.
        }
        catch (Exception exception)
        {
            System.Diagnostics.Trace.TraceWarning(
                "Lapis subtitle selection failed: {0}", exception);
        }
        finally
        {
            if (ReferenceEquals(_subtitleChoiceCancellation, cancellation))
            {
                _subtitleChoiceCancellation = null;
                cancellation.Dispose();
            }
        }
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
        SyncOverlayTopmost();
        SyncBounds();

        // A native Seek advances the epoch and retires the subtitle worker.
        // Old completed tracks self-hide immediately; reload the selected
        // embedded stream at most once per new playback generation.
        var playback = _player.PlaybackSnapshot;
        if (_subtitlePicker.SelectedItem is SubtitleChoice
            { Track.Kind: TextSubtitleSourceKind.EmbeddedText } selected &&
            _embeddedSelectionEpoch != playback.Epoch.Value &&
            playback.Transport is (Lapis.Contracts.Playback.TransportState.Ready or
                Lapis.Contracts.Playback.TransportState.Playing or
                Lapis.Contracts.Playback.TransportState.Paused or
                Lapis.Contracts.Playback.TransportState.Ended))
        {
            SetSubtitleTrack(SidecarSubtitleTrack.Empty);
            _ = LoadSubtitleChoiceAsync(selected);
        }

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

        // Do not disappear beneath a stationary cursor, scrub gesture, or
        // keyboard focus: playback chrome stays usable until interaction ends.
        if (_player.TransportText != "Playing" || _sidebarOpen ||
            _subtitlePicker.IsDropDownOpen || _controls.IsPointerOver ||
            _header.IsPointerOver || _controls.IsKeyboardFocusWithin ||
            _header.IsKeyboardFocusWithin)
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

    private void OwnerActivated(object? sender, EventArgs args) =>
        QueueOverlayZOrderSync();

    private void OwnerDeactivated(object? sender, EventArgs args) =>
        QueueOverlayZOrderSync();

    private void QueueOverlayZOrderSync() =>
        Dispatcher.UIThread.Post(
            () => SetOverlayTopmost(ShouldOverlayBeTopmost()),
            DispatcherPriority.Background);

    private void SetOverlayTopmost(bool topmost)
    {
        if (_disposed) return;
        Topmost = topmost;
        if (!OperatingSystem.IsWindows() ||
            TryGetPlatformHandle()?.Handle is not { } handle ||
            handle == 0)
            return;

        var insertAfter = topmost ? HwndTopmost : HwndNotTopmost;
        if (!SetWindowPos(handle, insertAfter, 0, 0, 0, 0,
                SwpNoMove | SwpNoSize | SwpNoActivate))
        {
            ReportTopmostSyncFailure(new Win32Exception(
                Marshal.GetLastWin32Error(),
                "Could not synchronize the watch overlay z-order with its owner."));
            return;
        }

        var isTopmost = (GetWindowLongPtr(handle, GwlExStyle).ToInt64() &
            WsExTopmost) != 0;
        if (isTopmost != topmost)
        {
            ReportTopmostSyncFailure(new InvalidOperationException(
                "The watch overlay did not adopt the requested z-order."));
            return;
        }
        _topmostSyncFailureReported = false;
    }

    private void SyncOverlayTopmost()
    {
        if (_disposed) return;
        if (!OperatingSystem.IsWindows() ||
            TryGetPlatformHandle()?.Handle is not { } handle ||
            handle == 0)
            return;

        var isTopmost = (GetWindowLongPtr(handle, GwlExStyle).ToInt64() &
            WsExTopmost) != 0;
        var shouldBeTopmost = ShouldOverlayBeTopmost();
        if (isTopmost != shouldBeTopmost)
            SetOverlayTopmost(shouldBeTopmost);
    }

    private void ReportTopmostSyncFailure(Exception exception)
    {
        if (_topmostSyncFailureReported) return;
        _topmostSyncFailureReported = true;
        System.Diagnostics.Trace.TraceError(
            "Lapis watch overlay z-order synchronization failed: {0}",
            exception);
    }

    private bool ShouldOverlayBeTopmost() =>
        _owner.IsActive || IsOwnerForeground();

    private bool IsOwnerForeground()
    {
        var owner = _owner.TryGetPlatformHandle()?.Handle ?? 0;
        var foreground = GetForegroundWindow();
        if (owner == 0 || foreground == 0) return false;
        var ownerRoot = GetAncestor(owner, GaRootOwner);
        return ownerRoot != 0 &&
            ownerRoot == GetAncestor(foreground, GaRootOwner);
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
        // Let the subtitle selector own its keys, particularly Escape and
        // Left/Right. Do not seek or leave Watch while choosing a subtitle.
        if (_subtitlePicker.IsDropDownOpen ||
            args.Source is ComboBox or ComboBoxItem)
            return;
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

    private void OnBackSeekPressed(object? sender, PointerPressedEventArgs args)
    {
        if (!args.GetCurrentPoint(_backSeek).Properties.IsLeftButtonPressed)
            return;
        _backSeekPressed = true;
        _seekButtonPressedAt = DateTime.UtcNow;
        ++_seekPressGeneration;
        NotifyPointerActivity();
    }

    private void OnForwardSeekPressed(object? sender, PointerPressedEventArgs args)
    {
        if (!args.GetCurrentPoint(_forwardSeek).Properties.IsLeftButtonPressed)
            return;
        _forwardSeekPressed = true;
        _seekButtonPressedAt = DateTime.UtcNow;
        var generation = ++_seekPressGeneration;
        _fastForwardStartTask = null;
        _fastForwardActive = false;
        _fastForwardDelay?.Cancel();
        _fastForwardDelay?.Dispose();
        _fastForwardDelay = new CancellationTokenSource();
        _ = StartFastForwardAfterDelayAsync(
            generation, _fastForwardDelay.Token);
        NotifyPointerActivity();
    }

    private async Task StartFastForwardAfterDelayAsync(
        int generation, CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(SeekHoldThreshold, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return;
        }

        Dispatcher.UIThread.Post(() =>
        {
            if (_disposed || !_forwardSeekPressed ||
                generation != _seekPressGeneration ||
                _fastForwardStartTask is not null)
                return;
            _fastForwardStartTask = _player.SetTemporaryFastForwardAsync(true);
            _ = CompleteFastForwardStartAsync(generation, _fastForwardStartTask);
        });
    }

    private async Task CompleteFastForwardStartAsync(
        int generation, Task<bool> startTask)
    {
        var started = await startTask;
        if (generation != _seekPressGeneration || !_forwardSeekPressed || _disposed)
        {
            if (started) await _player.SetTemporaryFastForwardAsync(false);
            return;
        }
        _fastForwardActive = started;
    }

    private async void OnBackSeekReleased(object? sender, PointerReleasedEventArgs args)
    {
        if (!_backSeekPressed || _seekButtonPressedAt is not { } pressedAt)
            return;
        _backSeekPressed = false;
        _seekButtonPressedAt = null;
        var elapsed = DateTime.UtcNow - pressedAt;
        if (elapsed < SeekHoldThreshold) return;

        args.Handled = true;
        var current = _player.PlaybackSnapshot.Position.ToTimeSpan();
        var target = current - TimeSpan.FromSeconds(elapsed.TotalSeconds * 4);
        await _player.SeekToAsync(target);
        NotifyPointerActivity();
    }

    private async void OnForwardSeekReleased(object? sender, PointerReleasedEventArgs args)
    {
        if (!_forwardSeekPressed || _seekButtonPressedAt is not { } pressedAt)
            return;
        _forwardSeekPressed = false;
        _seekButtonPressedAt = null;
        var elapsed = DateTime.UtcNow - pressedAt;
        _fastForwardDelay?.Cancel();
        if (elapsed < SeekHoldThreshold)
        {
            _fastForwardDelay?.Dispose();
            _fastForwardDelay = null;
            return;
        }

        args.Handled = true;
        var generation = _seekPressGeneration;
        var startTask = _fastForwardStartTask;
        if (startTask is null)
        {
            startTask = _player.SetTemporaryFastForwardAsync(true);
            _fastForwardStartTask = startTask;
        }

        var started = await startTask;
        if (started)
        {
            await _player.SetTemporaryFastForwardAsync(false);
            _fastForwardActive = false;
        }
        else
        {
            var current = _player.PlaybackSnapshot.Position.ToTimeSpan();
            var target = current + TimeSpan.FromSeconds(elapsed.TotalSeconds * 2);
            await _player.SeekToAsync(target);
        }
        if (generation == _seekPressGeneration)
        {
            _fastForwardStartTask = null;
            _fastForwardDelay?.Dispose();
            _fastForwardDelay = null;
        }
        NotifyPointerActivity();
    }

    private void OnSeekButtonCaptureLost(
        object? sender, PointerCaptureLostEventArgs args)
    {
        if (ReferenceEquals(sender, _forwardSeek) && _forwardSeekPressed)
        {
            _forwardSeekPressed = false;
            _seekButtonPressedAt = null;
            _fastForwardDelay?.Cancel();
            ++_seekPressGeneration;
            _ = RestoreFastForwardAfterPendingStartAsync();
        }
        else if (ReferenceEquals(sender, _backSeek))
        {
            _backSeekPressed = false;
            _seekButtonPressedAt = null;
        }
    }

    private async Task RestoreFastForwardAfterPendingStartAsync()
    {
        if (_fastForwardStartTask is { } startTask &&
            await startTask &&
            !_disposed)
            await _player.SetTemporaryFastForwardAsync(false);
        _fastForwardActive = false;
    }

    private void DisposeOverlay()
    {
        if (_disposed) return;
        _disposed = true;
        if (_subtitleTrack is IAsyncDisposable retire)
            _ = RetireSubtitleAsync(retire);
        _subtitleTrack = SidecarSubtitleTrack.Empty;
        ++_subtitleChoiceGeneration;
        _subtitleChoiceCancellation?.Cancel();
        _subtitleChoiceCancellation?.Dispose();
        _subtitleChoiceCancellation = null;
        _subtitlePicker.SelectionChanged -= SubtitlePickerChanged;
        _clock.Stop();
        _clock.Tick -= ClockTick;
        if (OperatingSystem.IsWindows())
        {
            _owner.Activated -= OwnerActivated;
            _owner.Deactivated -= OwnerDeactivated;
        }
        _player.PropertyChanged -= PlayerChanged;
        KeyDown -= OnOverlayKeyDown;
        _player.CancelScrub();
        _fastForwardDelay?.Cancel();
        _fastForwardDelay?.Dispose();
        _fastForwardDelay = null;
        ++_seekPressGeneration;
        _ = RestoreFastForwardAfterPendingStartAsync();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CursorPoint
    {
        public int X;
        public int Y;
    }

    private static readonly nint HwndTopmost = new(-1);
    private static readonly nint HwndNotTopmost = new(-2);
    private const int GwlExStyle = -20;
    private const uint GaRootOwner = 3;
    private const long WsExTopmost = 0x00000008;
    private const uint SwpNoSize = 0x0001;
    private const uint SwpNoMove = 0x0002;
    private const uint SwpNoActivate = 0x0010;

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        nint handle, nint insertAfter, int x, int y, int width, int height,
        uint flags);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW", SetLastError = true)]
    private static extern nint GetWindowLongPtr(nint handle, int index);

    [DllImport("user32.dll")]
    private static extern nint GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern nint GetAncestor(nint handle, uint flags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out CursorPoint point);
}
