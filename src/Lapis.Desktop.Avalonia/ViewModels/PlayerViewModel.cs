using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Windows.Input;
using Avalonia.Threading;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Avalonia.Services;

namespace Lapis.Desktop.Avalonia.ViewModels;

/// <summary>
/// Avalonia-free playback intent/state apart from the UI-dispatcher boundary.
/// All commands delegate to the existing serialized DesktopPlaybackHost actor;
/// the ViewModel never handles HWND, packet frames or GPU resources.
/// </summary>
public sealed class PlayerViewModel : INotifyPropertyChanged, IAsyncDisposable
{
    private readonly IPlayerPlaybackService _player;
    private readonly CancellationTokenSource _lifetime = new();
    private readonly SemaphoreSlim _commands = new(1, 1);
    private Task? _observations;
    private MediaLocator? _currentMedia;
    private bool _busy;
    private bool _disposed;
    private int _refreshPending;

    private string _mediaTitle = "尚未開啟影片";
    private string _statusText = "選擇影片或未加密 BDMV 資料夾開始播放";
    private string _positionText = "00:00 / --:--";
    private TransportState _transport = TransportState.Closed;
    private double _seekPositionSeconds;
    private double _seekDurationSeconds = 1;
    private bool _scrubbing;
    private PlaybackEpoch _scrubEpoch;

    public PlayerViewModel(IPlayerPlaybackService player)
    {
        _player = player ?? throw new ArgumentNullException(nameof(player));
        ToggleCommand = new AsyncCommand(
            () => RunAsync(ToggleCoreAsync));
        StopCommand = new AsyncCommand(
            () => RunAsync(() => _player.CloseAsync(_lifetime.Token)));
        BackCommand = new AsyncCommand(() => RunAsync(() => SeekRelativeAsync(-10)));
        ForwardCommand = new AsyncCommand(() => RunAsync(() => SeekRelativeAsync(10)));
    }

    // Observational only. The service/host still fences every optional
    // embedded subtitle request against this exact SessionId and Epoch.
    public PlaybackSnapshot PlaybackSnapshot => _player.Snapshot;
    public IEmbeddedSubtitlePlaybackService? EmbeddedSubtitlePlayback =>
        _player as IEmbeddedSubtitlePlaybackService;

    public event PropertyChangedEventHandler? PropertyChanged;

    public ICommand ToggleCommand { get; }
    public ICommand StopCommand { get; }
    public ICommand BackCommand { get; }
    public ICommand ForwardCommand { get; }

    public string MediaTitle
    {
        get => _mediaTitle;
        private set => SetProperty(ref _mediaTitle, value);
    }
    public string StatusText
    {
        get => _statusText;
        private set => SetProperty(ref _statusText, value);
    }
    public string PositionText
    {
        get => _positionText;
        private set => SetProperty(ref _positionText, value);
    }
    public string TransportText => _transport.ToString();
    public string PlayPauseLabel => _transport == TransportState.Playing
        ? "暫停" : _transport == TransportState.Ended ? "重播" : "播放";
    public bool CanTransport => !_busy && _currentMedia is not null &&
        _transport is TransportState.Ready or TransportState.Playing
            or TransportState.Paused or TransportState.Ended;
    public bool CanSeek => !_busy && _currentMedia is not null &&
        _transport is TransportState.Ready or TransportState.Playing
            or TransportState.Paused;
    public bool CanScrub => CanSeek &&
        _player.Snapshot.Duration is { Nanoseconds: > 0 };

    /// <summary>
    /// Timeline is a preview only while dragging. Native snapshots never
    /// fight the thumb, and changing Value never implicitly seeks.
    /// </summary>
    public double SeekPositionSeconds
    {
        get => _seekPositionSeconds;
        set
        {
            if (!_scrubbing || !double.IsFinite(value)) return;
            var preview = Math.Clamp(value, 0, _seekDurationSeconds);
            SetProperty(ref _seekPositionSeconds, preview);
            PositionText = $"{FormatTime(TimeSpan.FromSeconds(preview))} / " +
                FormatTime(TimeSpan.FromSeconds(_seekDurationSeconds));
        }
    }

    public double SeekDurationSeconds => _seekDurationSeconds;

    public void BeginScrub()
    {
        if (_disposed || !CanScrub) return;
        _scrubbing = true;
        _scrubEpoch = _player.Snapshot.Epoch;
    }

    public void CancelScrub()
    {
        if (!_scrubbing) return;
        _scrubbing = false;
        ApplySnapshot(_player.Snapshot);
    }

    /// <summary>
    /// Commit at most one Seek per drag-release. A transport discontinuity
    /// during dragging invalidates the preview rather than seeking an old
    /// epoch after another command has already moved playback.
    /// </summary>
    public async Task CommitScrubAsync()
    {
        if (!_scrubbing) return;
        _scrubbing = false;
        var target = TimeSpan.FromSeconds(_seekPositionSeconds);
        if (_disposed || !CanScrub || _scrubEpoch != _player.Snapshot.Epoch)
        {
            if (!_disposed) ApplySnapshot(_player.Snapshot);
            return;
        }

        try
        {
            await RunAsync(() => _player.SeekAsync(target, _lifetime.Token));
        }
        finally
        {
            if (!_disposed) ApplySnapshot(_player.Snapshot);
        }
    }

    public Task OpenAsync(MediaLocator locator)
    {
        ArgumentNullException.ThrowIfNull(locator);
        return RunAsync(async () =>
        {
            await _player.OpenAsync(locator, _lifetime.Token);
            _currentMedia = locator;
            MediaTitle = locator.Kind == MediaLocatorKind.Directory
                ? $"BDMV · {Path.GetFileName(Path.TrimEndingDirectorySeparator(locator.Value))}"
                : Path.GetFileName(locator.Value);
            ApplySnapshot(_player.Snapshot);
            StartObservingIfNeeded();
        });
    }

    /// <summary>Stop/detach media before leaving the watch screen.</summary>
    public Task CloseAsync() => RunAsync(() => _player.CloseAsync(_lifetime.Token));

    public void ReportError(string message) => StatusText = message;

    private async Task ToggleCoreAsync()
    {
        if (_currentMedia is null) return;
        if (_transport == TransportState.Ended)
            await _player.OpenAsync(_currentMedia, _lifetime.Token);
        else if (_transport is TransportState.Ready or TransportState.Playing
                 or TransportState.Paused)
            await _player.ToggleAsync(_lifetime.Token);
    }

    private Task SeekRelativeAsync(int deltaSeconds)
    {
        if (!CanSeek)
            return Task.CompletedTask;
        var position = _player.Snapshot.Position.ToTimeSpan() +
                       TimeSpan.FromSeconds(deltaSeconds);
        var duration = _player.Snapshot.Duration?.ToTimeSpan();
        if (position < TimeSpan.Zero) position = TimeSpan.Zero;
        if (duration is not null && position > duration.Value)
            position = duration.Value;
        return _player.SeekAsync(position, _lifetime.Token);
    }

    private async Task RunAsync(Func<Task> operation)
    {
        if (_disposed) return;
        try
        {
            await _commands.WaitAsync(_lifetime.Token);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
            return;
        }
        try
        {
            if (_disposed) return;
            _busy = true;
            OnPropertyChanged(nameof(CanTransport));
            OnPropertyChanged(nameof(CanSeek));
            OnPropertyChanged(nameof(CanScrub));
            await operation();
            ApplySnapshot(_player.Snapshot);
            StatusText = _player.Snapshot.LastError is { } error
                ? $"播放錯誤：{error.Domain}/{error.Code}"
                : $"播放狀態：{_player.Snapshot.Transport}";
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
            // Normal shutdown.
        }
        catch (Exception exception)
        {
            Trace.TraceError("Lapis Avalonia playback command: {0}", exception);
            StatusText = $"無法執行：{exception.Message}";
        }
        finally
        {
            _busy = false;
            OnPropertyChanged(nameof(CanTransport));
            OnPropertyChanged(nameof(CanSeek));
            OnPropertyChanged(nameof(CanScrub));
            _commands.Release();
        }
    }

    private void StartObservingIfNeeded()
    {
        if (_observations is not null) return;
        _observations = Task.Run(async () =>
        {
            try
            {
                await foreach (var snapshot in _player.ObserveSnapshotsAsync(
                    _lifetime.Token).WithCancellation(_lifetime.Token))
                {
                    // Drop stale generations and never enqueue one dispatcher
                    // callback for every media snapshot under heavy UI load.
                    if (snapshot.Epoch.Value < _player.Snapshot.Epoch.Value)
                        continue;
                    if (Interlocked.Exchange(ref _refreshPending, 1) == 0)
                    {
                        Dispatcher.UIThread.Post(() =>
                        {
                            Interlocked.Exchange(ref _refreshPending, 0);
                            if (!_disposed) ApplySnapshot(_player.Snapshot);
                        });
                    }
                }
            }
            catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) {}
            catch (ObjectDisposedException) when (_lifetime.IsCancellationRequested) {}
            catch (Exception exception)
            {
                Trace.TraceError("Lapis native snapshot observation: {0}", exception);
            }
        });
    }

    private void ApplySnapshot(PlaybackSnapshot snapshot)
    {
        _transport = snapshot.Transport;
        OnPropertyChanged(nameof(TransportText));
        OnPropertyChanged(nameof(PlayPauseLabel));
        OnPropertyChanged(nameof(CanTransport));
        OnPropertyChanged(nameof(CanSeek));
        OnPropertyChanged(nameof(CanScrub));
        var time = snapshot.Position.ToTimeSpan();
        var duration = snapshot.Duration?.ToTimeSpan();
        var maxSeconds = Math.Max(1, duration?.TotalSeconds ?? 1);
        SetProperty(ref _seekDurationSeconds, maxSeconds);
        OnPropertyChanged(nameof(SeekDurationSeconds));
        if (!_scrubbing)
        {
            SetProperty(ref _seekPositionSeconds,
                Math.Clamp(time.TotalSeconds, 0, maxSeconds));
            PositionText = $"{FormatTime(time)} / " +
                (duration.HasValue ? FormatTime(duration.Value) : "--:--");
        }
    }

    private static string FormatTime(TimeSpan time) =>
        time.TotalHours >= 1
            ? $"{(long)time.TotalHours:00}:{time.Minutes:00}:{time.Seconds:00}"
            : $"{(long)time.TotalMinutes:00}:{time.Seconds:00}";

    private void SetProperty<T>(ref T field, T value,
        [CallerMemberName] string propertyName = "")
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        OnPropertyChanged(propertyName);
    }

    private void OnPropertyChanged(string propertyName) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        _lifetime.Cancel();
        await _commands.WaitAsync();
        try
        {
            await _player.DisposeAsync();
        }
        finally
        {
            _commands.Release();
        }
        if (_observations is not null) await _observations;
        _commands.Dispose();
        _lifetime.Dispose();
    }
}
