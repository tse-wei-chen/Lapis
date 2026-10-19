using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Lapis.Desktop.WinForms;
using Xunit;

namespace Lapis.Desktop.WinForms.Tests;

/// <summary>
/// Runs on a real Windows STA, without loading FFmpeg or a D3D device.
/// Verifies the persistent HWND is parked before Panel WM_DESTROY can
/// recursively destroy Win32 child windows.
/// </summary>
public sealed class VideoSurfacePanelTests
{
    [Fact]
    public async Task OutputHandleSurvivesRepeatedPanelRecreationAndThenReleases()
    {
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                using var form = new Form { Size = new Size(640, 480) };
                var panel = new VideoSurfacePanel { Dock = DockStyle.Fill };
                form.Controls.Add(panel);
                _ = form.Handle;
                _ = panel.Handle;

                nint outputHwnd = panel.GetOutputHandle();
                Assert.True(IsWindow(outputHwnd));
                Assert.Equal(panel.Handle, GetParent(outputHwnd));

                for (var i = 0; i < 40; i++)
                {
                    panel.RecreateForDiagnostics();
                    panel.ClientSize = new Size(200 + (i % 20), 150 + (i % 30));
                    Assert.Equal(outputHwnd, panel.GetOutputHandle());
                    Assert.Equal(panel.Handle, GetParent(outputHwnd));
                    Assert.True(IsWindow(outputHwnd));
                }

                Assert.Equal(40, panel.ParentHandleRecreations);
                // Disposal ordering is required by the real player: native
                // playback must already be stopped before this destruction.
                panel.ReleaseOutputAfterRuntimeDisposed();
                Assert.False(IsWindow(outputHwnd));

                // Idempotence: Stop/form teardown should never double-destroy.
                panel.ReleaseOutputAfterRuntimeDisposed();
                form.Dispose();
                completion.TrySetResult();
            }
            catch (Exception exception)
            {
                completion.TrySetException(exception);
            }
        })
        {
            IsBackground = true
        };

        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        await completion.Task.WaitAsync(TimeSpan.FromSeconds(30));
        Assert.True(thread.Join(TimeSpan.FromSeconds(5)));
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint hwnd);

    [DllImport("user32.dll")]
    private static extern nint GetParent(nint hwnd);
}
