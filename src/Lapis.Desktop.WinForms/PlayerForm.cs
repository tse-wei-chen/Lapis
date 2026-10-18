using System.Drawing;
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
    private readonly Panel _video = new()
    {
        Dock = DockStyle.Fill,
        BackColor = Color.Black,
        Margin = new Padding(12, 12, 12, 6),
        TabStop = false
    };
    private readonly Button _open = NewButton("開啟影片");
    private readonly Button _playPause = NewButton("播放");
    private readonly Button _closeMedia = NewButton("停止");
    private readonly Button _seek = NewButton("跳至");
    private readonly TrackBar _progress = new()
    {
        Dock = DockStyle.Fill,
        AutoSize = false,
        TickStyle = TickStyle.None,
        Minimum = 0,
        Maximum = 1000,
        Enabled = false,
        Height = 40,
        Margin = new Padding(12, 3, 12, 3)
    };
    private bool _scrubbing;
    private readonly NumericUpDown _seekSeconds = new()
    {
        Minimum = 0,
        Maximum = 86400,
        DecimalPlaces = 0,
        Width = 90,
        TextAlign = HorizontalAlignment.Right
    };
    private readonly Label _state = new()
    {
        AutoSize = false,
        Dock = DockStyle.Fill,
        ForeColor = Color.Gainsboro,
        Text = "選擇本機影片，即可開始播放。",
        TextAlign = ContentAlignment.MiddleLeft,
        Padding = new Padding(14, 0, 0, 0)
    };

    private DesktopPlaybackHost? _host;
    private string? _openedFile;
    private bool _busy;
    private bool _closing;
    private bool _closeApproved;

    public PlayerForm()
    {
        Text = "Lapis — Desktop Player";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(720, 470);
        Size = new Size(1100, 710);
        BackColor = Color.FromArgb(22, 24, 30);
        ForeColor = Color.WhiteSmoke;
        Font = new Font("Segoe UI", 10f);
        KeyPreview = true;
        AllowDrop = true;

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            RowCount = 4,
            ColumnCount = 1,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100f));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 40f));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 68f));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 36f));

        var toolbar = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            BackColor = Color.FromArgb(32, 35, 43),
            Padding = new Padding(12, 11, 12, 4),
            Margin = Padding.Empty
        };
        toolbar.Controls.AddRange(
            new Control[] { _open, _playPause, _closeMedia, _seek, _seekSeconds });
        toolbar.Controls.Add(new Label
        {
            Text = "秒",
            ForeColor = Color.Silver,
            TextAlign = ContentAlignment.MiddleLeft,
            AutoSize = false,
            Size = new Size(36, 38),
            Padding = new Padding(2, 0, 0, 0)
        });

        var statusBar = new Panel
        {
            Dock = DockStyle.Fill,
            Margin = Padding.Empty,
            BackColor = Color.FromArgb(32, 35, 43)
        };
        statusBar.Controls.Add(_state);

        root.Controls.Add(_video, 0, 0);
        root.Controls.Add(_progress, 0, 1);
        root.Controls.Add(toolbar, 0, 2);
        root.Controls.Add(statusBar, 0, 3);
        Controls.Add(root);

        _open.Click += async (_, _) => await ChooseFileAsync();
        _playPause.Click += async (_, _) => await RunUiCommandAsync(
            async (host, token) => await host.TogglePlayPauseAsync(token));
        _closeMedia.Click += async (_, _) => await RunUiCommandAsync(
            async (host, token) =>
            {
                await host.CloseMediaAsync(token);
                _openedFile = null;
                Text = "Lapis — Desktop Player";
            });
        _seek.Click += async (_, _) => await RunUiCommandAsync(
            async (host, token) => await host.SeekAsync(
                TimeSpan.FromSeconds((double)_seekSeconds.Value), token));
        _progress.MouseDown += (_, _) => _scrubbing = true;
        _progress.MouseUp += async (_, _) =>
        {
            _scrubbing = false;
            var duration = _host?.Snapshot.Duration;
            if (duration is { Nanoseconds: > 0 } && !_closing && !_busy)
            {
                // MediaTime is canonical nanoseconds; use ticks to retain
                // sub-second seek precision without clock simulation in UI.
                var targetTicks = (long)(duration.Value.Nanoseconds / 100.0 *
                    _progress.Value / _progress.Maximum);
                await RunUiCommandAsync(async (host, token) =>
                    await host.SeekAsync(TimeSpan.FromTicks(targetTicks), token));
            }
        };

        DragEnter += OnDragEnter;
        DragDrop += async (_, eventArgs) =>
        {
            if (eventArgs.Data?.GetData(DataFormats.FileDrop) is string[] files &&
                files.Length != 0)
            {
                await OpenFileAsync(files[0]);
            }
        };
        KeyDown += OnKeyDown;
        FormClosing += OnClosing;
        RefreshControls();
    }

    private static Button NewButton(string caption) => new()
    {
        Text = caption,
        Width = 100,
        Height = 38,
        FlatStyle = FlatStyle.Flat,
        Margin = new Padding(0, 0, 9, 0),
        BackColor = Color.FromArgb(55, 62, 77),
        ForeColor = Color.WhiteSmoke,
        UseVisualStyleBackColor = false
    };

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

    private Task OpenFileAsync(string file) => RunUiCommandAsync(
        async (host, token) =>
        {
            // Access the Control.Handle on the UI thread only. The HWND is a
            // borrowed display target: we keep it alive through native Dispose.
            var hwnd = _video.Handle;
            await host.OpenAndPlayAsync(file, hwnd, token);
            _openedFile = file;
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
                BeginInvoke(new Action(() =>
                {
                    if (!_closing && !IsDisposed)
                    {
                        DisplaySnapshot(snapshot);
                    }
                }));
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
        if (!_scrubbing)
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
        _state.Text = snapshot.LastError is null
            ? $"狀態：{snapshot.Transport}　|　{positionText} / {durationText}" +
              (_openedFile is null ? "" : $"　|　{Path.GetFileName(_openedFile)}")
            : $"錯誤：{(snapshot.LastError.Context ?? snapshot.LastError.Code)}";
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
        _playPause.Enabled = canInteract &&
            state is TransportState.Ready or TransportState.Playing or TransportState.Paused;
        _playPause.Text = state == TransportState.Playing ? "暫停" : "播放";
        _closeMedia.Enabled = canInteract && state != TransportState.Closed;
        _seek.Enabled = canInteract &&
            state is TransportState.Ready or TransportState.Playing or TransportState.Paused;
        _seekSeconds.Enabled = _seek.Enabled;
        _progress.Enabled = canInteract &&
            _host?.Snapshot.Duration is { Nanoseconds: > 0 } &&
            state is (TransportState.Ready or TransportState.Playing or
                TransportState.Paused or TransportState.Ended);
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
        _state.Text = $"錯誤：{message}";
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
        if (e.Control && e.KeyCode == Keys.O)
        {
            e.Handled = true;
            await ChooseFileAsync();
        }
        else if (e.KeyCode == Keys.Space &&
                 !_seekSeconds.Focused && !_closing && !_busy &&
                 _playPause.Enabled)
        {
            e.Handled = true;
            await RunUiCommandAsync(
                async (host, token) => await host.TogglePlayPauseAsync(token));
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

        _lifetime.Cancel();
        _closeApproved = true;
        BeginInvoke(new Action(Close));
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _lifetime.Dispose();
            _uiCommands.Dispose();
        }
        base.Dispose(disposing);
    }
}
