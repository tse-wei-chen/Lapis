using System.Drawing;
using System.Text.Json;
using System.Windows.Forms;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Hosting;
using Lapis.Runtime.Native;

namespace Lapis.Desktop.WinForms;

/// <summary>
/// Minimal Windows shell. Media decoding/presentation runs in MediaRuntime,
/// transport ordering runs in DesktopPlaybackHost, and this Form only owns HWND
/// lifetime plus user input. No native interop or D3D types are used here.
/// </summary>
public sealed class PlayerForm : Form
{
    private readonly SemaphoreSlim _uiCommands = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    // All visual composition lives in PlayerChrome; this Form only adapts
    // user actions to framework-neutral DesktopPlaybackHost commands.
    private readonly PlayerChrome _chrome = new();
    private readonly System.Windows.Forms.Timer _chromeIdleTimer = new()
    {
        Interval = 200
    };
    private readonly System.Windows.Forms.Timer _videoClickTimer = new()
    {
        Interval = SystemInformation.DoubleClickTime
    };
    private DateTime _lastMotionUtc = DateTime.UtcNow;
    private Point _previousPointer = Cursor.Position;

    private VideoSurfacePanel _video => _chrome.Video;
    private Button _open => _chrome.OpenButton;
    private Button _playPause => _chrome.PlayButton;
    private Button _closeMedia => _chrome.StopButton;
    private Button _seek => _chrome.SeekButton;
    private Button _diagnostics => _chrome.DiagnosticsButton;
    private PlaybackSeekBar _progress => _chrome.Progress;
    private NumericUpDown _seekSeconds => _chrome.SeekSeconds;

    private DesktopPlaybackHost? _host;
    private NativePlaybackRuntime? _nativeRuntime;
    private string? _openedFile;
    private bool _busy;
    private bool _closing;
    private bool _closeApproved;
    private bool _fullScreen;
    private FormBorderStyle _restoreBorderStyle;
    private FormWindowState _restoreWindowState;
    private Rectangle _restoreBounds;

    public PlayerForm()
    {
        Text = "Lapis — Desktop Player";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(850, 530);
        Size = new Size(1200, 760);
        BackColor = Color.FromArgb(11, 16, 25);
        ForeColor = Color.WhiteSmoke;
        Font = new Font("Segoe UI", 10f);
        KeyPreview = true;
        AllowDrop = true;
        Controls.Add(_chrome);

        _open.Click += async (_, _) => await ChooseFileAsync();
        _chrome.ChooseMediaButton.Click += async (_, _) => await ChooseFileAsync();
        _diagnostics.Click += async (_, _) => await ExportDiagnosticsAsync();
        _playPause.Click += async (_, _) => await TogglePlayPauseAsync();
        _closeMedia.Click += async (_, _) => await RunUiCommandAsync(
            async (host, token) =>
            {
                await host.CloseMediaAsync(token);
                _openedFile = null;
                Text = "Lapis — Desktop Player";
                _chrome.SetMedia(null);
                _chrome.SetTimeline("00:00", "--:--");
            });
        _seek.Click += async (_, _) => await RunUiCommandAsync(
            async (host, token) => await host.SeekAsync(
                TimeSpan.FromSeconds((double)_seekSeconds.Value), token));
        _chrome.BackButton.Click += async (_, _) => await SeekRelativeAsync(-10);
        _chrome.ForwardButton.Click += async (_, _) => await SeekRelativeAsync(10);
        _chrome.FullScreenButton.Click += (_, _) => ToggleFullScreen();
        _chrome.TheaterButton.Click += (_, _) => ToggleTheater();

        // The custom, YouTube-style timeline emits only on an actual release
        // or keyboard seek. Native position updates never override dragging.
        _progress.SeekRequested += position =>
        {
            RegisterInteraction();
            _ = RunUiCommandAsync(async (host, token) =>
                await host.SeekAsync(position, token));
        };

        // Video surface belongs to a persistent HWND. Single click toggles
        // playback; double click enters/exits fullscreen without accidentally
        // also toggling pause on the first of its two clicks.
        _video.MouseClick += (_, e) =>
        {
            if (e.Button == MouseButtons.Left && !_closing && !_busy)
            {
                _videoClickTimer.Stop();
                _videoClickTimer.Start();
            }
        };
        _video.MouseDoubleClick += (_, e) =>
        {
            if (e.Button == MouseButtons.Left)
            {
                _videoClickTimer.Stop();
                ToggleFullScreen();
                RegisterInteraction();
            }
        };
        _videoClickTimer.Tick += async (_, _) =>
        {
            _videoClickTimer.Stop();
            if (!_closing && !_busy && _playPause.Enabled)
            {
                await TogglePlayPauseAsync();
            }
        };

        _chromeIdleTimer.Tick += (_, _) => UpdateChromeVisibility();
        Shown += (_, _) =>
        {
            _previousPointer = Cursor.Position;
            _chromeIdleTimer.Start();
        };

        DragEnter += OnDragEnter;
        DragDrop += OnDragDrop;
        // WinForms routes drag events to the child under the cursor, not
        // necessarily to the Form. Both the idle overlay and video area
        // therefore accept the exact same OpenFile command.
        AttachFileDropHandlers(_video);
        // The idle overlay contains nested layout panels and labels. Register
        // their HWNDs too, or an external drop over the visible instructions
        // would silently be ignored by WinForms OLE drag routing.
        AttachFileDropHandlers(_chrome.EmptySurface);

        KeyDown += OnKeyDown;
        FormClosing += OnClosing;
        Resize += (_, _) =>
        {
            _chrome.SetCompactHint(ClientSize.Width < 1050);
            RegisterInteraction();
        };
        _chrome.SetCompactHint(ClientSize.Width < 1050);
        RefreshControls();
    }

    private void AttachFileDropHandlers(Control surface)
    {
        surface.AllowDrop = true;
        surface.DragEnter += OnDragEnter;
        surface.DragDrop += OnDragDrop;
        foreach (Control child in surface.Controls)
        {
            AttachFileDropHandlers(child);
        }
    }

    private void RegisterInteraction()
    {
        _lastMotionUtc = DateTime.UtcNow;
        _chrome.SetControlsVisible(true);
    }

    private void UpdateChromeVisibility()
    {
        if (_closing || IsDisposed)
        {
            return;
        }

        var cursor = Cursor.Position;
        if (cursor != _previousPointer)
        {
            _previousPointer = cursor;
            RegisterInteraction();
        }

        var immersive = _fullScreen || _chrome.TheaterMode;
        if (!immersive)
        {
            _chrome.SetControlsVisible(true);
            return;
        }

        // Controls stay accessible when paused, busy, scrubbing or when the
        // user is actually pointing at the control deck.
        if (_busy ||
            _host?.Snapshot.Transport != TransportState.Playing ||
            _progress.IsScrubbing ||
            _progress.Focused ||
            _seekSeconds.Focused ||
            _chrome.IsCursorOverTransport(cursor))
        {
            RegisterInteraction();
            return;
        }

        // A short idle timeout feels like YouTube while preserving the Win32
        // child HWND ordering: we're collapsing a sibling deck, not drawing
        // WinForms controls across a GPU-owned child HWND.
        _chrome.SetControlsVisible(
            DateTime.UtcNow - _lastMotionUtc < TimeSpan.FromSeconds(3));
    }

    private void ToggleTheater()
    {
        if (_closing || _busy)
        {
            return;
        }
        _chrome.SetTheaterMode(!_chrome.TheaterMode);
        RegisterInteraction();
    }

    private async Task TogglePlayPauseAsync()
    {
        // Native EOF is terminal: replay must open a new decoder generation.
        if (_host?.Snapshot.Transport == TransportState.Ended &&
            _openedFile is { } replayFile)
        {
            await OpenFileAsync(replayFile);
        }
        else
        {
            await RunUiCommandAsync(
                async (host, token) => await host.TogglePlayPauseAsync(token));
        }
    }

    private Task SeekRelativeAsync(int seconds)
    {
        if (_closing || _busy || _host is null)
        {
            return Task.CompletedTask;
        }
        return RunUiCommandAsync(async (host, token) =>
        {
            var snapshot = host.Snapshot;
            var position = snapshot.Position.ToTimeSpan() +
                TimeSpan.FromSeconds(seconds);
            if (position < TimeSpan.Zero)
            {
                position = TimeSpan.Zero;
            }
            if (snapshot.Duration is { Nanoseconds: > 0 } duration)
            {
                var maximum = duration.ToTimeSpan();
                if (position > maximum)
                {
                    position = maximum;
                }
            }
            await host.SeekAsync(position, token);
        });
    }

    private async void OnDragDrop(object? sender, DragEventArgs eventArgs)
    {
        if (!_closing && !_busy &&
            eventArgs.Data?.GetData(DataFormats.FileDrop) is string[] files &&
            files.Length > 0 && File.Exists(files[0]))
        {
            await OpenFileAsync(files[0]);
        }
    }

    private void ToggleFullScreen()
    {
        if (_closing || _busy)
        {
            return;
        }
        if (!_fullScreen)
        {
            RegisterInteraction();
            _restoreBorderStyle = FormBorderStyle;
            _restoreWindowState = WindowState;
            _restoreBounds = Bounds;
            _fullScreen = true;
            _chrome.SetFullScreen(true);
            WindowState = FormWindowState.Normal;
            FormBorderStyle = FormBorderStyle.None;
            WindowState = FormWindowState.Maximized;
        }
        else
        {
            RegisterInteraction();
            _chrome.SetFullScreen(false);
            FormBorderStyle = _restoreBorderStyle;
            WindowState = FormWindowState.Normal;
            Bounds = _restoreBounds;
            WindowState = _restoreWindowState;
            _fullScreen = false;
        }
        RegisterInteraction();
    }

    private async Task ChooseFileAsync()
    {
        if (_busy || _closing)
        {
            return;
        }
        using var dialog = new OpenFileDialog
        {
            Title = "開啟本機影片",
            Filter = "影片檔案|*.mkv;*.mp4;*.m2ts;*.mts;*.ts;*.avi;*.mov;*.webm;*.y4m|所有檔案|*.*",
            CheckFileExists = true,
            Multiselect = false
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            await OpenFileAsync(dialog.FileName);
        }
    }

    private Task ExportDiagnosticsAsync() =>
        RunUiCommandAsync(async (_, token) =>
        {
            if (_nativeRuntime is null)
            {
                throw new InvalidOperationException(
                    "請先開啟一部影片，再匯出診斷報告。");
            }

            using var dialog = new SaveFileDialog
            {
                Title = "匯出 Lapis 診斷報告",
                Filter = "JSON 診斷報告|*.json",
                FileName = $"lapis-diagnostics-{DateTimeOffset.UtcNow:yyyyMMdd-HHmmss}.json",
                AddExtension = true,
                DefaultExt = "json",
                OverwritePrompt = true
            };
            if (dialog.ShowDialog(this) != DialogResult.OK)
            {
                return;
            }

            // Native records never include the opened media's path.
            var captured = await Task.Run(
                () => _nativeRuntime.CaptureDiagnostics(), token);
            var document = new
            {
                schemaVersion = 1,
                capturedAtUtc = DateTimeOffset.UtcNow,
                application = "Lapis Desktop",
                playbackAbiVersion = 2,
                diagnosticsApiVersion = 1,
                operatingSystem = Environment.OSVersion.VersionString,
                dotnetVersion = Environment.Version.ToString(),
                transport = _host?.Snapshot.Transport.ToString(),
                videoPanelRecreations = _video.ParentHandleRecreations,
                journalCapacity = 128,
                captured.OverwrittenEvents,
                captured.Events
            };
            var json = JsonSerializer.Serialize(
                document, new JsonSerializerOptions { WriteIndented = true });
            await File.WriteAllTextAsync(dialog.FileName, json, token);
        });

    private Task OpenFileAsync(string file) => RunUiCommandAsync(
        async (host, token) =>
        {
            // The rendering HWND is independently owned and parked across
            // WinForms Panel handle recreation. Do not lend the Panel HWND.
            var hwnd = _video.GetOutputHandle();
            _openedFile = null;
            Text = "Lapis — Desktop Player";
            _chrome.SetMedia(null);
            await host.OpenAndPlayAsync(file, hwnd, token);
            _openedFile = file;
            _chrome.SetMedia(Path.GetFileName(file));
            Text = $"Lapis — {Path.GetFileName(file)}";
        });

    private async Task RunUiCommandAsync(
        Func<DesktopPlaybackHost, CancellationToken, Task> action)
    {
        try
        {
            await _uiCommands.WaitAsync();
            try
            {
                if (_closing)
                {
                    return;
                }
                _busy = true;
                RefreshControls();

                var host = await EnsureHostAsync();
                await action(host, _lifetime.Token);
                if (!_closing)
                {
                    RefreshControls();
                }
            }
            finally
            {
                _busy = false;
                _uiCommands.Release();
                if (!_closing && !IsDisposed)
                {
                    if (_host is not null)
                    {
                        DisplaySnapshot(_host.Snapshot);
                    }
                    else
                    {
                        RefreshControls();
                    }
                }
            }
        }
        catch (OperationCanceledException) when (_closing)
        {
            // Window shutdown owns the final native release.
        }
        catch (Exception exception)
        {
            if (!_closing && !IsDisposed)
            {
                ShowFailure(exception);
            }
        }
    }

    private async Task<DesktopPlaybackHost> EnsureHostAsync()
    {
        if (_host is not null)
        {
            return _host;
        }

        // Native library initialization may load FFmpeg and create a runtime;
        // keep that synchronous P/Invoke work off the UI dispatcher.
        var runtime = await Task.Run(
            static () => new NativePlaybackRuntime(), _lifetime.Token);
        var host = new DesktopPlaybackHost(runtime, runtime);
        _nativeRuntime = runtime;
        _host = host;
        _ = ObserveSnapshotsAsync(host, _lifetime.Token);
        return host;
    }

    private async Task ObserveSnapshotsAsync(
        DesktopPlaybackHost host,
        CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var snapshot in host.ObserveSnapshotsAsync(cancellationToken))
            {
                if (_closing || IsDisposed || !IsHandleCreated)
                {
                    continue;
                }
                try
                {
                    BeginInvoke(new Action(() =>
                    {
                        if (!_closing && !IsDisposed && IsHandleCreated)
                        {
                            DisplaySnapshot(snapshot);
                        }
                    }));
                }
                catch (InvalidOperationException) when (!IsHandleCreated || _closing || IsDisposed)
                {
                    // The form handle can disappear between the probe and
                    // BeginInvoke during PMv2/control recreation.
                }
                catch (ObjectDisposedException) when (_closing || IsDisposed)
                {
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException) when (_closing)
        {
        }
    }

    private void DisplaySnapshot(PlaybackSnapshot snapshot)
    {
        if (_busy)
        {
            return;
        }
        var duration = snapshot.Duration;
        _progress.Duration = duration?.ToTimeSpan() ?? TimeSpan.Zero;
        if (!_progress.IsScrubbing)
        {
            _progress.Value = duration is { Nanoseconds: > 0 }
                ? Math.Clamp((int)Math.Round(
                    (double)snapshot.Position.Nanoseconds /
                    duration.Value.Nanoseconds * _progress.Maximum),
                    0, _progress.Maximum)
                : 0;
        }
        var positionText = FormatPosition(snapshot.Position.ToTimeSpan());
        var durationText = duration.HasValue
            ? FormatPosition(duration.Value.ToTimeSpan())
            : "--:--";
        _chrome.SetTimeline(positionText, durationText);
        var stateLabel = snapshot.Transport switch
        {
            TransportState.Closed => "準備就緒",
            TransportState.Opening => "正在開啟",
            TransportState.Ready => "已就緒",
            TransportState.Playing => "播放中",
            TransportState.Paused => "已暫停",
            TransportState.Seeking => "跳轉中",
            TransportState.Ended => "播放完成",
            TransportState.Faulted => "播放發生問題",
            _ => snapshot.Transport.ToString()
        };
        _chrome.SetState(snapshot.LastError is null
            ? $"{stateLabel}  ·  {positionText} / {durationText}" +
              (_openedFile is null ? "" : $"  ·  {Path.GetFileName(_openedFile)}")
            : $"播放錯誤：{(snapshot.LastError.Context ?? snapshot.LastError.Code)}",
            snapshot.LastError is not null);
        RefreshControls();
    }

    private static string FormatPosition(TimeSpan time) =>
        time.TotalHours >= 1
            ? $"{(long)time.TotalHours:00}:{time.Minutes:00}:{time.Seconds:00}"
            : $"{time.Minutes:00}:{time.Seconds:00}";

    private void RefreshControls()
    {
        var state = _host?.Snapshot.Transport ?? TransportState.Closed;
        var canInteract = !_busy && !_closing;
        _open.Enabled = canInteract;
        _chrome.ChooseMediaButton.Enabled = canInteract;
        _diagnostics.Enabled = canInteract && _nativeRuntime is not null;
        _playPause.Enabled = canInteract &&
            (state is TransportState.Ready or TransportState.Playing or TransportState.Paused ||
             (state == TransportState.Ended && _openedFile is not null));
        _playPause.Text = state == TransportState.Playing ? "Ⅱ  暫停" :
            state == TransportState.Ended ? "↻  重播" : "▶  播放";
        _closeMedia.Enabled = canInteract && state != TransportState.Closed;
        var canSeek = canInteract &&
            state is TransportState.Ready or TransportState.Playing or TransportState.Paused;
        _seek.Enabled = canSeek;
        _seekSeconds.Enabled = canSeek;
        _chrome.BackButton.Enabled = canSeek;
        _chrome.ForwardButton.Enabled = canSeek;
        _progress.Enabled = canSeek &&
            _host?.Snapshot.Duration is { Nanoseconds: > 0 };
        _chrome.FullScreenButton.Enabled = canInteract;
        _chrome.TheaterButton.Enabled = canInteract;
        UpdateChromeVisibility();
    }

    private void ShowFailure(Exception exception)
    {
        var message = exception switch
        {
            DllNotFoundException =>
                "找不到 Lapis.MediaRuntime.dll。請先依 README 建置 Native Release，" +
                "再執行 Desktop Host。詳見 docs/development/desktop-host.md。",
            EntryPointNotFoundException =>
                "Native DLL 與 Managed ABI 版本不相容，請重新建置 Native MediaRuntime。",
            _ => exception.Message
        };
        _chrome.SetState($"錯誤：{message}", true);
        MessageBox.Show(this, message, "Lapis 播放錯誤",
            MessageBoxButtons.OK, MessageBoxIcon.Error);
    }

    private void OnDragEnter(object? sender, DragEventArgs e)
    {
        e.Effect = !_closing && !_busy &&
            e.Data?.GetDataPresent(DataFormats.FileDrop) == true
            ? DragDropEffects.Copy
            : DragDropEffects.None;
    }

    private async void OnKeyDown(object? sender, KeyEventArgs e)
    {
        // Ignore transport shortcuts while editing an absolute seek value.
        // Modifier shortcuts remain available even if another child has focus.
        var editingTime = _seekSeconds.Focused;
        var editingTimeline = _progress.Focused;

        if (e.KeyCode is Keys.F11 or Keys.F && !e.Control && !e.Alt &&
            !_closing && !editingTime)
        {
            e.Handled = true;
            e.SuppressKeyPress = true;
            ToggleFullScreen();
        }
        else if (e.KeyCode == Keys.Escape && _fullScreen)
        {
            e.Handled = true;
            e.SuppressKeyPress = true;
            ToggleFullScreen();
        }
        else if (e.Control && e.KeyCode == Keys.O)
        {
            e.Handled = true;
            e.SuppressKeyPress = true;
            await ChooseFileAsync();
        }
        else if (e.Control && e.Shift && e.KeyCode == Keys.H &&
                 !_closing && !_busy && _host is not null)
        {
            // Stable native HWND cannot change during an active UI
            // recreation, regardless of theater/fullscreen mode.
            e.Handled = true;
            e.SuppressKeyPress = true;
            await RunUiCommandAsync((_, _) =>
            {
                var outputHwnd = _video.GetOutputHandle();
                _video.RecreateForDiagnostics();
                if (_video.GetOutputHandle() != outputHwnd)
                {
                    throw new InvalidOperationException(
                        "Video output HWND changed during panel recreation.");
                }
                return Task.CompletedTask;
            });
        }
        else if (e.KeyCode == Keys.T && !e.Control && !e.Alt &&
                 !_closing && !editingTime)
        {
            e.Handled = true;
            e.SuppressKeyPress = true;
            ToggleTheater();
        }
        else if (!editingTime && !editingTimeline && !_closing &&
                 !_busy && _chrome.BackButton.Enabled &&
                 e.KeyCode is Keys.Left or Keys.Right or Keys.J or Keys.L)
        {
            e.Handled = true;
            e.SuppressKeyPress = true;
            RegisterInteraction();
            await SeekRelativeAsync(e.KeyCode is Keys.Left or Keys.J ? -10 : 10);
        }
        else if (!editingTime && !_closing && !_busy &&
                 _playPause.Enabled &&
                 e.KeyCode is Keys.Space or Keys.K)
        {
            e.Handled = true;
            e.SuppressKeyPress = true;
            RegisterInteraction();
            await TogglePlayPauseAsync();
        }
        else if (e.KeyCode == Keys.D0 && !e.Control && !e.Alt &&
                 !editingTime && !_busy && _chrome.BackButton.Enabled)
        {
            e.Handled = true;
            e.SuppressKeyPress = true;
            RegisterInteraction();
            await RunUiCommandAsync(async (host, token) =>
                await host.SeekAsync(TimeSpan.Zero, token));
        }
    }

    private async void OnClosing(object? sender, FormClosingEventArgs e)
    {
        if (_closeApproved)
        {
            return;
        }

        // Cancellation of FormClosing postpones destruction of the borrowed
        // HWND. The native render worker must release its DXGI swapchain first.
        e.Cancel = true;
        if (_closing)
        {
            return;
        }

        _closing = true;
        _chromeIdleTimer.Stop();
        _videoClickTimer.Stop();
        RefreshControls();
        await _uiCommands.WaitAsync();
        try
        {
            if (_host is not null)
            {
                await _host.DisposeAsync();
            }
        }
        catch (Exception exception)
        {
            // Never destroy a potentially borrowed HWND when native teardown
            // was unsuccessful. Keep the Form alive for retry/diagnosis.
            _closing = false;
            ShowFailure(exception);
            RefreshControls();
            return;
        }
        finally
        {
            _uiCommands.Release();
        }

        // DXGI render worker has joined and detached before the island
        // destroys its independently owned output HWND.
        _video.ReleaseOutputAfterRuntimeDisposed();
        _lifetime.Cancel();
        _closeApproved = true;
        BeginInvoke(new Action(Close));
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _chromeIdleTimer.Dispose();
            _videoClickTimer.Dispose();
            _lifetime.Dispose();
            _uiCommands.Dispose();
        }
        base.Dispose(disposing);
    }
}
