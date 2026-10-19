using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace Lapis.Desktop.WinForms;

/// <summary>
/// Timeline built only from transport snapshot position/duration. We do not
/// invent buffered ranges, thumbnails or chapter data that the runtime has not
/// provided. User drag previews a timestamp; releasing sends a single seek.
/// </summary>
internal sealed class PlaybackSeekBar : Control
{
    private const int Inset = 8;
    private static readonly Color BackgroundTrack = Color.FromArgb(84, 95, 111);
    private static readonly Color PlayedTrack = Color.FromArgb(237, 71, 86);
    private static readonly Color Thumb = Color.White;
    private static readonly Color PreviewBackground = Color.FromArgb(35, 40, 51);

    private int _value;
    private int _hoverValue;
    private bool _hovering;
    private bool _dragging;
    private TimeSpan _duration;

    public const int MaximumValue = 1000;

    public event Action<TimeSpan>? SeekRequested;

    public PlaybackSeekBar()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer |
                 ControlStyles.Selectable, true);
        Size = new Size(320, 39);
        MinimumSize = new Size(60, 32);
        Cursor = Cursors.Hand;
        TabStop = true;
        AccessibleName = "播放進度";
        AccessibleDescription = "滑鼠點擊或拖動搜尋；鍵盤方向鍵以五秒移動";
        BackColor = Color.FromArgb(19, 27, 40);
        ForeColor = Color.White;
    }

    public int Maximum => MaximumValue;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Value
    {
        get => _value;
        set
        {
            if (_dragging)
            {
                return; // native snapshot must not steal an active drag
            }
            SetValue(value);
        }
    }

    public bool IsScrubbing => _dragging;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public TimeSpan Duration
    {
        get => _duration;
        set
        {
            _duration = value > TimeSpan.Zero ? value : TimeSpan.Zero;
            Invalidate();
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var left = Inset;
        var usableWidth = Math.Max(1, ClientSize.Width - Inset * 2);
        var mid = ClientSize.Height - 11;
        var barHeight = _hovering || _dragging || Focused ? 6 : 4;
        var y = mid - barHeight / 2;

        using (var trackBrush = new SolidBrush(BackgroundTrack))
        {
            g.FillRectangle(trackBrush, left, y, usableWidth, barHeight);
        }
        var playedWidth = Math.Clamp(
            (int)Math.Round(usableWidth * (double)_value / MaximumValue),
            0, usableWidth);
        using (var playedBrush = new SolidBrush(PlayedTrack))
        {
            g.FillRectangle(playedBrush, left, y, playedWidth, barHeight);
        }
        if (Enabled && (_hovering || _dragging || Focused))
        {
            using var dotBrush = new SolidBrush(Thumb);
            var x = left + playedWidth;
            g.FillEllipse(dotBrush, x - 6, mid - 6, 12, 12);
        }

        if (Enabled && (_hovering || _dragging) && _duration > TimeSpan.Zero)
        {
            var preview = TimeSpan.FromTicks(
                (long)(_duration.Ticks * ((double)_hoverValue / MaximumValue)));
            var caption = FormatTimestamp(preview);
            using var font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
            var size = TextRenderer.MeasureText(caption, font);
            var bubbleWidth = size.Width + 12;
            var bubbleX = Math.Clamp(
                left + (int)Math.Round(usableWidth *
                    ((double)_hoverValue / MaximumValue)) - bubbleWidth / 2,
                0, Math.Max(0, ClientSize.Width - bubbleWidth));
            var bubble = new Rectangle(bubbleX, 0, bubbleWidth, 23);
            using var backdrop = new SolidBrush(PreviewBackground);
            g.FillRectangle(backdrop, bubble);
            TextRenderer.DrawText(g, caption, font, bubble,
                Color.White, TextFormatFlags.HorizontalCenter |
                TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
        }

        if (Focused && ShowFocusCues)
        {
            ControlPaint.DrawFocusRectangle(
                g, new Rectangle(0, ClientSize.Height - 3, ClientSize.Width, 3));
        }
    }

    protected override void OnMouseEnter(EventArgs e)
    {
        base.OnMouseEnter(e);
        _hovering = true;
        Invalidate();
    }

    protected override void OnMouseLeave(EventArgs e)
    {
        base.OnMouseLeave(e);
        _hovering = false;
        if (!_dragging)
        {
            Invalidate();
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        _hoverValue = ValueForX(e.X);
        if (_dragging)
        {
            SetValue(_hoverValue);
        }
        Invalidate();
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (!Enabled || e.Button != MouseButtons.Left ||
            _duration <= TimeSpan.Zero)
        {
            return;
        }
        Focus();
        _dragging = true;
        Capture = true;
        _hoverValue = ValueForX(e.X);
        SetValue(_hoverValue);
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        // Commit before base processing releases Win32 mouse capture: a
        // capture-lost callback must not turn a real mouse-up into Cancel.
        if (_dragging && e.Button == MouseButtons.Left)
        {
            _hoverValue = ValueForX(e.X);
            SetValue(_hoverValue);
            FinishDrag(commit: true);
        }
        base.OnMouseUp(e);
    }

    protected override void OnMouseCaptureChanged(EventArgs e)
    {
        base.OnMouseCaptureChanged(e);
        if (_dragging && !Capture)
        {
            FinishDrag(commit: false);
        }
    }

    private void FinishDrag(bool commit)
    {
        // Set this before releasing capture, to avoid reentrant cancellation.
        _dragging = false;
        Capture = false;
        Invalidate();
        if (commit && Enabled && _duration > TimeSpan.Zero)
        {
            SeekRequested?.Invoke(AsTime(_value));
        }
    }

    protected override bool IsInputKey(Keys keyData)
    {
        var key = keyData & Keys.KeyCode;
        return key is Keys.Left or Keys.Right or Keys.Home or Keys.End ||
               base.IsInputKey(keyData);
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (!Enabled || _duration <= TimeSpan.Zero)
        {
            return;
        }

        var step = Math.Max(1,
            (int)Math.Round(MaximumValue * 5.0 / _duration.TotalSeconds));
        var next = e.KeyCode switch
        {
            Keys.Left => _value - step,
            Keys.Right => _value + step,
            Keys.Home => 0,
            Keys.End => MaximumValue,
            _ => _value
        };
        if (e.KeyCode is not (Keys.Left or Keys.Right or Keys.Home or Keys.End))
        {
            return;
        }
        e.Handled = true;
        e.SuppressKeyPress = true;
        SetValue(next);
        SeekRequested?.Invoke(AsTime(_value));
    }

    private int ValueForX(int x) =>
        (int)Math.Round(Math.Clamp(
            ((double)(x - Inset) / Math.Max(1, Width - 2 * Inset)), 0, 1) *
            MaximumValue);

    private TimeSpan AsTime(int value) =>
        TimeSpan.FromTicks((long)(_duration.Ticks *
            ((double)value / MaximumValue)));

    private void SetValue(int value)
    {
        var next = Math.Clamp(value, 0, MaximumValue);
        if (next == _value)
        {
            return;
        }
        _value = next;
        Invalidate();
    }

    private static string FormatTimestamp(TimeSpan time) =>
        time.TotalHours >= 1
            ? $"{(long)time.TotalHours:00}:{time.Minutes:00}:{time.Seconds:00}"
            : $"{(long)time.TotalMinutes:00}:{time.Seconds:00}";
}
