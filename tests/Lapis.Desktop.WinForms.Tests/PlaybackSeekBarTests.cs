using System.Drawing;
using System.Threading;
using System.Windows.Forms;
using Lapis.Desktop.WinForms;
using Xunit;

namespace Lapis.Desktop.WinForms.Tests;

/// <summary>
/// The timeline is a native-free UI component. Rendering and snapshot-driven
/// values can be tested without loading FFmpeg, a GPU or an audio endpoint.
/// </summary>
public sealed class PlaybackSeekBarTests
{
    [Fact]
    public async Task TimelineClampsSnapshotProgressAndPaintsWithUnknownDuration()
    {
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                using var form = new Form { Size = new Size(760, 220) };
                using var seek = new PlaybackSeekBar
                {
                    Dock = DockStyle.Top,
                    Enabled = true,
                    Duration = TimeSpan.FromMinutes(10)
                };
                form.Controls.Add(seek);
                form.Show();

                Assert.Equal(1000, seek.Maximum);
                Assert.False(seek.IsScrubbing);
                Assert.Equal("播放進度", seek.AccessibleName);

                seek.Value = -250;
                Assert.Equal(0, seek.Value);
                seek.Value = 500;
                Assert.Equal(500, seek.Value);
                seek.Value = 5000;
                Assert.Equal(1000, seek.Value);

                using var canvas = new Bitmap(seek.Width, seek.Height);
                seek.DrawToBitmap(canvas,
                    new Rectangle(0, 0, canvas.Width, canvas.Height));

                seek.Duration = TimeSpan.Zero;
                Assert.Equal(TimeSpan.Zero, seek.Duration);
                seek.Invalidate();
                seek.DrawToBitmap(canvas,
                    new Rectangle(0, 0, canvas.Width, canvas.Height));

                completion.TrySetResult();
            }
            catch (Exception e)
            {
                completion.TrySetException(e);
            }
        }) { IsBackground = true };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        await completion.Task.WaitAsync(TimeSpan.FromSeconds(30));
        Assert.True(thread.Join(TimeSpan.FromSeconds(5)));
    }
}
