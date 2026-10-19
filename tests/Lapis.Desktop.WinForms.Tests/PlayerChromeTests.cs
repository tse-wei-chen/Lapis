using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Lapis.Desktop.WinForms;
using Xunit;

namespace Lapis.Desktop.WinForms.Tests;

/// <summary>
/// Verify the shell's idle/playing/fullscreen visual state on an actual
/// Windows STA without requiring FFmpeg or a native DLL.
/// </summary>
public sealed class PlayerChromeTests
{
    [Fact]
    public async Task ShellTransitionsKeepTheOwnedVideoHwndAndAccessibleTransport()
    {
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                using var form = new Form { Size = new Size(1080, 720) };
                var chrome = new PlayerChrome();
                form.Controls.Add(chrome);
                // Visible is an effective WinForms parent/child property;
                // display the real shell before testing its idle overlay.
                form.Show();
                _ = chrome.Handle;

                Assert.True(chrome.EmptySurface.Visible);
                Assert.False(chrome.Progress.Enabled);
                Assert.NotNull(chrome.PlayButton.AccessibleName);
                Assert.Equal("開啟 BDMV", chrome.OpenBdmvButton.AccessibleName);
                Assert.Equal("BDMV 資料夾（預覽）",
                    chrome.ChooseBdmvButton.AccessibleName);
                Assert.True(chrome.OpenBdmvButton.Enabled);
                Assert.True(chrome.ChooseBdmvButton.Visible);
                Assert.NotNull(chrome.Progress.AccessibleName);
                Assert.Equal("00:00", chrome.CurrentPosition.Text);
                Assert.Equal("--:--", chrome.Duration.Text);

                chrome.SetMedia("movie-with-a-long-name.mkv");
                chrome.SetTimeline("02:14", "12:40");
                chrome.SetState("播放中 · 02:14 / 12:40");
                Assert.False(chrome.EmptySurface.Visible);
                Assert.Equal("02:14", chrome.CurrentPosition.Text);
                Assert.Equal("12:40", chrome.Duration.Text);
                Assert.Contains("播放中", chrome.State.Text);

                var output = chrome.Video.GetOutputHandle();
                Assert.True(IsWindow(output));
                Assert.Equal(chrome.Video.Handle, GetParent(output));

                // Immersive presentation changes WinForms layout only, not
                // the independently owned D3D11 target HWND.
                chrome.SetTheaterMode(true);
                Assert.True(chrome.TheaterMode);
                Assert.True(chrome.ControlsVisible);
                chrome.SetControlsVisible(false);
                Assert.False(chrome.ControlsVisible);
                Assert.Equal(output, chrome.Video.GetOutputHandle());
                Assert.Equal(chrome.Video.Handle, GetParent(output));

                chrome.SetControlsVisible(true);
                Assert.True(chrome.ControlsVisible);
                chrome.SetFullScreen(true);
                Assert.True(chrome.FullScreenMode);
                chrome.SetControlsVisible(false);
                Assert.False(chrome.ControlsVisible);
                Assert.True(IsWindow(output));
                Assert.Equal(output, chrome.Video.GetOutputHandle());

                // Returning to standard restores persistent control
                // visibility regardless of the prior idle state.
                chrome.SetFullScreen(false);
                chrome.SetTheaterMode(false);
                Assert.True(chrome.ControlsVisible);
                chrome.SetControlsVisible(false);
                Assert.True(chrome.ControlsVisible);
                chrome.SetCompactHint(true);
                chrome.SetCompactHint(false);
                Assert.Equal(output, chrome.Video.GetOutputHandle());
                Assert.Equal(chrome.Video.Handle, GetParent(output));

                chrome.SetMedia(null);
                Assert.True(chrome.EmptySurface.Visible);
                chrome.Video.ReleaseOutputAfterRuntimeDisposed();
                Assert.False(IsWindow(output));

                completion.TrySetResult();
            }
            catch (Exception error)
            {
                completion.TrySetException(error);
            }
        })
        {
            IsBackground = true,
            Name = "Lapis.PlayerChrome.STA"
        };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        await completion.Task.WaitAsync(TimeSpan.FromSeconds(30));
        Assert.True(thread.Join(TimeSpan.FromSeconds(5)));
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint handle);

    [DllImport("user32.dll")]
    private static extern nint GetParent(nint handle);
}
