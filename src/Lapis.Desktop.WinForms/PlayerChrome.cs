using System.Drawing;
using System.Windows.Forms;

namespace Lapis.Desktop.WinForms;

/// <summary>
/// Presentation-only Windows shell. The playback actor owns transport state,
/// VideoSurfacePanel owns the persistent HWND, and this view owns layout.
/// An AI/subtitle sidecar can be added here without touching MediaRuntime.
/// </summary>
internal sealed class PlayerChrome : UserControl
{
    private static readonly Color Canvas = Color.FromArgb(11, 16, 25);
    private static readonly Color Surface = Color.FromArgb(19, 27, 40);
    private static readonly Color Raised = Color.FromArgb(31, 43, 60);
    private static readonly Color Accent = Color.FromArgb(112, 158, 255);
    private static readonly Color PrimaryText = Color.FromArgb(241, 246, 255);
    private static readonly Color MutedText = Color.FromArgb(155, 173, 197);
    private static readonly Color ErrorText = Color.FromArgb(255, 145, 153);

    private readonly TableLayoutPanel _root = new();
    private readonly Panel _emptySurface = new();
    private readonly Label _nowPlaying = MakeLabel(
        "開啟影片，開始播放", 11f, PrimaryText);
    private readonly Label _shortcutHint = MakeLabel(
        "SPACE  播放／暫停    ←／→  快轉 10 秒    F11  全螢幕",
        9f, MutedText);
    private readonly ToolTip _tips = new();
    private bool _fullscreen;
    private bool _theater;
    private bool _controlsVisible = true;
    private bool _compact;
    private Panel? _stageFrame;
    private Control? _transportDeck;

    public VideoSurfacePanel Video { get; } = new()
    {
        Dock = DockStyle.Fill,
        BackColor = Color.Black,
        Margin = Padding.Empty,
        TabStop = false
    };

    public Panel EmptySurface => _emptySurface;

    public Button OpenButton { get; } = MakeButton("＋ 開啟影片", true, 128);
    public Button ChooseMediaButton { get; } = MakeButton("選擇本機影片", true, 176);
    public Button PlayButton { get; } = MakeButton("▶  播放", true, 95);
    public Button BackButton { get; } = MakeButton("↶ 10", false, 69);
    public Button ForwardButton { get; } = MakeButton("10 ↷", false, 69);
    public Button StopButton { get; } = MakeButton("■ 停止", false, 76);
    public Button SeekButton { get; } = MakeButton("跳至", false, 62);
    public Button FullScreenButton { get; } = MakeButton("⛶ 全螢幕", false, 106);
    public Button TheaterButton { get; } = MakeButton("▣ 劇院", false, 96);
    public Button DiagnosticsButton { get; } = MakeButton("匯出診斷", false, 112);

    public PlaybackSeekBar Progress { get; } = new()
    {
        Dock = DockStyle.Fill,
        Enabled = false,
        Margin = new Padding(0, 0, 0, 0)
    };

    public NumericUpDown SeekSeconds { get; } = new()
    {
        Minimum = 0,
        Maximum = 86400,
        DecimalPlaces = 0,
        Width = 76,
        Height = 32,
        TextAlign = HorizontalAlignment.Right,
        BorderStyle = BorderStyle.FixedSingle,
        BackColor = Raised,
        ForeColor = PrimaryText,
        AccessibleName = "跳至秒數",
        Margin = new Padding(4, 9, 7, 0)
    };

    public Label State { get; } = MakeLabel(
        "準備就緒 · 開啟影片或拖曳檔案開始播放", 9.5f, MutedText);

    public Label CurrentPosition { get; } = MakeLabel("00:00", 10f, PrimaryText);
    public Label Duration { get; } = MakeLabel("--:--", 10f, MutedText);

    public PlayerChrome()
    {
        Dock = DockStyle.Fill;
        BackColor = Canvas;
        ForeColor = PrimaryText;
        Font = new Font("Segoe UI", 10f);
        Margin = Padding.Empty;

        _root.Dock = DockStyle.Fill;
        _root.BackColor = Canvas;
        _root.Margin = Padding.Empty;
        _root.Padding = Padding.Empty;
        _root.ColumnCount = 1;
        _root.RowCount = 4;
        _root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        _root.RowStyles.Add(new RowStyle(SizeType.Absolute, 68));
        _root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        _root.RowStyles.Add(new RowStyle(SizeType.Absolute, 142));
        _root.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));

        _root.Controls.Add(BuildHeader(), 0, 0);
        _root.Controls.Add(BuildStage(), 0, 1);
        _root.Controls.Add(BuildTransport(), 0, 2);
        _root.Controls.Add(BuildStatusBar(), 0, 3);
        Controls.Add(_root);

        _tips.SetToolTip(PlayButton, "播放／暫停（Space）");
        _tips.SetToolTip(OpenButton, "開啟本機影片（Ctrl+O）");
        _tips.SetToolTip(BackButton, "向後跳 10 秒（←）");
        _tips.SetToolTip(ForwardButton, "向前跳 10 秒（→）");
        _tips.SetToolTip(FullScreenButton, "切換全螢幕（F11）");
        _tips.SetToolTip(TheaterButton, "劇院模式（T）：放大影片，隱藏周邊選單");
        _tips.SetToolTip(DiagnosticsButton, "輸出最近 128 筆 GPU 與播放診斷事件");
        _tips.SetToolTip(Progress, "滑鼠移動預覽時間，拖曳或按方向鍵跳轉");
        SetMedia(null);
    }

    public void SetMedia(string? filename)
    {
        var hasMedia = !string.IsNullOrEmpty(filename);
        _nowPlaying.Text = hasMedia ? filename! : "開啟影片，開始播放";
        _tips.SetToolTip(_nowPlaying, hasMedia ? filename : "尚未開啟媒體");
        _emptySurface.Visible = !hasMedia;
        if (!hasMedia)
        {
            _emptySurface.BringToFront();
        }
    }

    public void SetTimeline(string position, string duration)
    {
        CurrentPosition.Text = position;
        Duration.Text = duration;
    }

    public void SetState(string message, bool isError = false)
    {
        State.Text = message;
        State.ForeColor = isError ? ErrorText : MutedText;
        _tips.SetToolTip(State, message);
    }

    public bool IsCursorOverTransport(Point screenPoint) =>
        _transportDeck is { Visible: true } deck &&
        deck.RectangleToScreen(deck.ClientRectangle).Contains(screenPoint);

    public bool FullScreenMode => _fullscreen;
    public bool TheaterMode => _theater;
    public bool ControlsVisible => _controlsVisible;

    /// <summary>
    /// Unlike a true overlay, the controls are kept in a separate Win32-safe
    /// sibling region below the D3D child HWND. This avoids WinForms airspace
    /// z-order bugs while preserving the familiar YouTube watching behavior.
    /// </summary>
    public void SetFullScreen(bool enabled)
    {
        _fullscreen = enabled;
        _controlsVisible = true;
        ApplyViewingMode();
    }

    public void SetTheaterMode(bool enabled)
    {
        _theater = enabled;
        _controlsVisible = true;
        ApplyViewingMode();
    }

    public void SetControlsVisible(bool visible)
    {
        if (!_fullscreen && !_theater)
        {
            visible = true;
        }
        if (_controlsVisible == visible)
        {
            return;
        }
        _controlsVisible = visible;
        ApplyViewingMode();
    }

    public void SetCompactHint(bool compact)
    {
        _compact = compact;
        ApplyViewingMode();
    }

    private void ApplyViewingMode()
    {
        var immersive = _fullscreen || _theater;
        var footerVisible = !immersive || _controlsVisible;
        _root.SuspendLayout();
        try
        {
            _root.RowStyles[0].Height = immersive ? 0 : 68;
            _root.RowStyles[2].Height = footerVisible
                ? immersive ? 114 : 142 : 0;
            _root.RowStyles[3].Height = immersive ? 0 : 38;
            if (_stageFrame is not null)
            {
                _stageFrame.Padding = immersive
                    ? Padding.Empty : new Padding(11, 11, 11, 7);
            }
            _shortcutHint.Visible = !_compact && !immersive;
            FullScreenButton.Text = _fullscreen ? "⛶ 還原" : "⛶ 全螢幕";
            TheaterButton.Text = _theater ? "▣ 一般" : "▣ 劇院";
        }
        finally
        {
            _root.ResumeLayout(true);
        }
    }

    private Control BuildHeader()
    {
        var header = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            BackColor = Surface,
            Padding = new Padding(18, 10, 18, 10),
            Margin = Padding.Empty,
            ColumnCount = 5
        };
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 52));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 142));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 172));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 144));

        header.Controls.Add(new Label
        {
            Text = "◆",
            ForeColor = Color.FromArgb(14, 22, 36),
            BackColor = Accent,
            Font = new Font("Segoe UI Symbol", 18f, FontStyle.Bold),
            TextAlign = ContentAlignment.MiddleCenter,
            Size = new Size(40, 40),
            Margin = Padding.Empty,
            AccessibleName = "Lapis"
        }, 0, 0);

        var branding = new Panel
        {
            Dock = DockStyle.Fill,
            Margin = Padding.Empty
        };
        branding.Controls.Add(new Label
        {
            Text = "LAPIS",
            Dock = DockStyle.Top,
            Height = 26,
            ForeColor = PrimaryText,
            Font = new Font("Segoe UI", 15f, FontStyle.Bold),
            TextAlign = ContentAlignment.MiddleLeft
        });
        branding.Controls.Add(new Label
        {
            Text = "DESKTOP PLAYER",
            Dock = DockStyle.Bottom,
            Height = 15,
            ForeColor = MutedText,
            Font = new Font("Segoe UI", 7f, FontStyle.Bold),
            TextAlign = ContentAlignment.MiddleLeft
        });
        header.Controls.Add(branding, 1, 0);

        _nowPlaying.Dock = DockStyle.Fill;
        _nowPlaying.AutoEllipsis = true;
        _nowPlaying.Margin = new Padding(14, 0, 12, 0);
        _nowPlaying.TextAlign = ContentAlignment.MiddleLeft;
        header.Controls.Add(_nowPlaying, 2, 0);

        var futureAi = MakeLabel("AI 雙語字幕 · 規劃中", 9f, MutedText);
        futureAi.BackColor = Raised;
        futureAi.TextAlign = ContentAlignment.MiddleCenter;
        futureAi.Dock = DockStyle.Fill;
        futureAi.Margin = new Padding(9, 7, 9, 7);
        futureAi.AccessibleName = "AI 雙語字幕尚未實作";
        header.Controls.Add(futureAi, 3, 0);
        OpenButton.Dock = DockStyle.Fill;
        OpenButton.Margin = new Padding(6, 3, 0, 3);
        header.Controls.Add(OpenButton, 4, 0);
        return header;
    }

    private Control BuildStage()
    {
        var frame = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Canvas,
            Padding = new Padding(11, 11, 11, 7),
            Margin = Padding.Empty
        };
        _stageFrame = frame;
        var stage = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Black,
            Margin = Padding.Empty
        };
        stage.Controls.Add(Video);

        _emptySurface.Dock = DockStyle.Fill;
        _emptySurface.BackColor = Color.FromArgb(15, 23, 36);
        _emptySurface.Margin = Padding.Empty;
        var welcome = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
            Margin = Padding.Empty
        };
        welcome.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        welcome.RowStyles.Add(new RowStyle(SizeType.Percent, 32));
        welcome.RowStyles.Add(new RowStyle(SizeType.Absolute, 54));
        welcome.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
        welcome.RowStyles.Add(new RowStyle(SizeType.Absolute, 60));
        welcome.RowStyles.Add(new RowStyle(SizeType.Percent, 68));
        welcome.Controls.Add(new Label
        {
            Text = "讓每一幀，都值得專注",
            Font = new Font("Segoe UI", 22f, FontStyle.Bold),
            ForeColor = PrimaryText,
            TextAlign = ContentAlignment.BottomCenter,
            Dock = DockStyle.Fill
        }, 0, 1);
        welcome.Controls.Add(new Label
        {
            Text = "開啟本機影片，或把檔案直接拖曳到此處。",
            ForeColor = MutedText,
            Font = new Font("Segoe UI", 11f),
            TextAlign = ContentAlignment.MiddleCenter,
            Dock = DockStyle.Fill
        }, 0, 2);
        var buttonRow = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            AutoSize = false,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        var spaceLeft = new Panel { Width = 1, Height = 1 };
        buttonRow.Controls.Add(spaceLeft);
        buttonRow.Controls.Add(ChooseMediaButton);
        welcome.Controls.Add(buttonRow, 0, 3);
        buttonRow.Resize += (_, _) =>
            spaceLeft.Width = Math.Max(0, (buttonRow.ClientSize.Width -
                ChooseMediaButton.Width - ChooseMediaButton.Margin.Horizontal) / 2);
        _emptySurface.Controls.Add(welcome);
        stage.Controls.Add(_emptySurface);
        _emptySurface.BringToFront();
        frame.Controls.Add(stage);
        return frame;
    }

    private Control BuildTransport()
    {
        var deck = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.FromArgb(14, 18, 26),
            Padding = new Padding(18, 4, 18, 6),
            ColumnCount = 1,
            RowCount = 3,
            Margin = Padding.Empty
        };
        deck.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        _transportDeck = deck;
        deck.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        deck.RowStyles.Add(new RowStyle(SizeType.Absolute, 60));
        deck.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        var timeline = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            Margin = Padding.Empty
        };
        timeline.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 66));
        timeline.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        timeline.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 66));
        CurrentPosition.Dock = DockStyle.Fill;
        CurrentPosition.TextAlign = ContentAlignment.MiddleLeft;
        Duration.Dock = DockStyle.Fill;
        Duration.TextAlign = ContentAlignment.MiddleRight;
        timeline.Controls.Add(CurrentPosition, 0, 0);
        timeline.Controls.Add(Progress, 1, 0);
        timeline.Controls.Add(Duration, 2, 0);
        deck.Controls.Add(timeline, 0, 0);

        var transport = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            Margin = Padding.Empty
        };
        transport.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 43));
        transport.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 31));
        transport.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 26));

        var actions = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = Padding.Empty,
            Padding = new Padding(0, 3, 0, 0)
        };
        foreach (var action in new[]
                 { BackButton, PlayButton, ForwardButton, StopButton })
        {
            action.Height = 43;
            action.Margin = new Padding(0, 3, 8, 3);
            actions.Controls.Add(action);
        }
        transport.Controls.Add(actions, 0, 0);

        var jump = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = Padding.Empty,
            Padding = new Padding(0, 3, 0, 0)
        };
        jump.Controls.Add(new Label
        {
            Text = "跳到",
            Width = 38,
            Height = 45,
            ForeColor = MutedText,
            TextAlign = ContentAlignment.MiddleCenter
        });
        jump.Controls.Add(SeekSeconds);
        jump.Controls.Add(new Label
        {
            Text = "秒",
            Width = 25,
            Height = 45,
            ForeColor = MutedText,
            TextAlign = ContentAlignment.MiddleCenter
        });
        SeekButton.Height = 43;
        SeekButton.Margin = new Padding(0, 3, 0, 0);
        jump.Controls.Add(SeekButton);
        transport.Controls.Add(jump, 1, 0);

        var modes = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.RightToLeft,
            WrapContents = false,
            Padding = new Padding(0, 3, 0, 0),
            Margin = Padding.Empty
        };
        FullScreenButton.Height = 43;
        FullScreenButton.Margin = new Padding(5, 3, 0, 0);
        TheaterButton.Height = 43;
        TheaterButton.Margin = new Padding(5, 3, 0, 0);
        modes.Controls.Add(FullScreenButton);
        modes.Controls.Add(TheaterButton);
        transport.Controls.Add(modes, 2, 0);
        deck.Controls.Add(transport, 0, 1);

        _shortcutHint.Dock = DockStyle.Fill;
        _shortcutHint.TextAlign = ContentAlignment.MiddleLeft;
        _shortcutHint.Margin = Padding.Empty;
        deck.Controls.Add(_shortcutHint, 0, 2);
        return deck;
    }

    private Control BuildStatusBar()
    {
        var bar = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            BackColor = Canvas,
            ColumnCount = 2,
            Padding = new Padding(18, 1, 18, 2),
            Margin = Padding.Empty
        };
        bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        bar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
        State.Dock = DockStyle.Fill;
        State.AutoEllipsis = true;
        State.TextAlign = ContentAlignment.MiddleLeft;
        bar.Controls.Add(State, 0, 0);
        DiagnosticsButton.Dock = DockStyle.Fill;
        DiagnosticsButton.Margin = Padding.Empty;
        DiagnosticsButton.Font = new Font("Segoe UI", 8.5f);
        bar.Controls.Add(DiagnosticsButton, 1, 0);
        return bar;
    }

    private static Label MakeLabel(string text, float points, Color color) => new()
    {
        Text = text,
        ForeColor = color,
        Font = new Font("Segoe UI", points),
        BackColor = Color.Transparent,
        AutoSize = false,
        Margin = Padding.Empty,
        UseMnemonic = false
    };

    private static Button MakeButton(string text, bool accent, int width) => new()
    {
        Text = text,
        Width = width,
        Height = 42,
        FlatStyle = FlatStyle.Flat,
        FlatAppearance =
        {
            BorderSize = 0,
            MouseOverBackColor = accent
                ? Color.FromArgb(146, 181, 255)
                : Color.FromArgb(47, 62, 84),
            MouseDownBackColor = accent
                ? Color.FromArgb(84, 127, 232)
                : Color.FromArgb(29, 37, 54)
        },
        BackColor = accent ? Accent : Raised,
        ForeColor = accent ? Color.FromArgb(10, 19, 34) : PrimaryText,
        Font = new Font("Segoe UI", 10f, FontStyle.Bold),
        Cursor = Cursors.Hand,
        UseVisualStyleBackColor = false,
        TabStop = true,
        AccessibleName = text
    };

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _tips.Dispose();
        }
        base.Dispose(disposing);
    }
}
