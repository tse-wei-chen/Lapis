using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Hosting;
using Lapis.Runtime.Native;
using Xunit;

namespace Lapis.Desktop.WinForms.Tests;

/// <summary>
/// Opt-in Windows acceptance: the real WinForms HWND island, managed desktop
/// actor, native C ABI, FFmpeg decoder and D3D11 presentation all participate.
/// Ordinary managed tests stay independent of the native DLL. The native CI
/// job enables this only AFTER building Lapis.MediaRuntime.dll.
/// </summary>
public sealed class NativeWinFormsPlaybackAcceptanceTests
{
    [Fact]
    public async Task LiveNativePlaybackSurvivesPanelRecreationSeekReplayAndClose()
    {
        if (Environment.GetEnvironmentVariable("LAPIS_NATIVE_UI_ACCEPTANCE") != "1")
        {
            return;
        }

        var mediaPath = Environment.GetEnvironmentVariable("LAPIS_NATIVE_UI_MEDIA");
        Assert.False(string.IsNullOrWhiteSpace(mediaPath),
            "LAPIS_NATIVE_UI_MEDIA must point to the compiled native CI fixture.");
        Assert.True(File.Exists(mediaPath), "Native UI test fixture is missing.");
        Assert.True(OperatingSystem.IsWindows(), "Real WinForms acceptance requires Windows.");

        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(55));
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var thread = new Thread(() =>
        {
            try
            {
                using var form = new Form
                {
                    Text = "Lapis Native UI Acceptance",
                    Size = new Size(720, 520),
                    StartPosition = FormStartPosition.Manual,
                    Location = new Point(90, 90)
                };
                var panel = new VideoSurfacePanel { Dock = DockStyle.Fill };
                form.Controls.Add(panel);

                form.Shown += async (_, _) =>
                {
                    try
                    {
                        await ExerciseLivePipelineAsync(
                            panel, mediaPath!, cancellation.Token);
                        completion.TrySetResult();
                    }
                    catch (Exception exception)
                    {
                        completion.TrySetException(exception);
                    }
                    finally
                    {
                        // Async awaits resume on the WinForms message loop.
                        // The native render worker has already been joined by
                        // the host before this Form is allowed to close.
                        form.Close();
                    }
                };

                System.Windows.Forms.Application.Run(form);
                if (!completion.Task.IsCompleted)
                {
                    completion.TrySetException(
                        new InvalidOperationException("WinForms message loop exited early."));
                }
            }
            catch (Exception exception)
            {
                completion.TrySetException(exception);
            }
        })
        {
            IsBackground = true,
            Name = "Lapis.NativeWinFormsAcceptance.STA"
        };

        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        await completion.Task.WaitAsync(TimeSpan.FromSeconds(60));
        Assert.True(thread.Join(TimeSpan.FromSeconds(5)),
            "WinForms UI thread did not exit after native teardown.");
    }

    private static async Task ExerciseLivePipelineAsync(
        VideoSurfacePanel panel,
        string mediaPath,
        CancellationToken cancellationToken)
    {
        // The UI thread owns HWND creation and reparenting. Native wait paths
        // are serialized by DesktopPlaybackHost and never block this dispatcher.
        Assert.Equal(ApartmentState.STA, Thread.CurrentThread.GetApartmentState());
        var uiThreadId = Environment.CurrentManagedThreadId;
        var output = panel.GetOutputHandle();
        Assert.True(IsWindow(output));
        Assert.Equal(panel.Handle, GetParent(output));

        var native = new NativePlaybackRuntime();
        var host = new DesktopPlaybackHost(native, native);
        var nativeDisposed = false;

        try
        {
            await host.OpenAndPlayAsync(mediaPath, output, cancellationToken);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);

            // Unlike an HWND-only unit test, this proves the production
            // renderer actually attached a D3D11 swap-chain to the HWND.
            await WaitUntilAsync(
                () => native.CaptureDiagnostics().Events.Any(
                    entry => entry.Kind == 3), // diagnostics ABI v1: SwapChainCreated
                TimeSpan.FromSeconds(10), cancellationToken);

            var initialEpoch = host.Snapshot.Epoch;
            for (var index = 0; index < 20; index++)
            {
                Assert.Equal(uiThreadId, Environment.CurrentManagedThreadId);
                panel.RecreateForDiagnostics();
                panel.ClientSize = new Size(
                    450 + (index % 5) * 17, 290 + (index % 4) * 13);

                Assert.Equal(output, panel.GetOutputHandle());
                Assert.True(IsWindow(output));
                Assert.Equal(panel.Handle, GetParent(output));
                Assert.NotEqual(TransportState.Faulted, host.Snapshot.Transport);
                await Task.Delay(12, cancellationToken);
            }

            Assert.Equal(20, panel.ParentHandleRecreations);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);

            await host.SeekAsync(TimeSpan.FromMilliseconds(250), cancellationToken);
            Assert.True(host.Snapshot.Epoch.Value > initialEpoch.Value);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);

            await WaitUntilAsync(
                () => host.Snapshot.Transport is TransportState.Ended
                    or TransportState.Faulted,
                TimeSpan.FromSeconds(12), cancellationToken);
            Assert.Equal(TransportState.Ended, host.Snapshot.Transport);

            // The same managed host/runtime must replay after EOF with a new
            // native decoder generation and keep the same output HWND lease.
            var afterEof = host.Snapshot.Epoch;
            await host.OpenAndPlayAsync(mediaPath, output, cancellationToken);
            Assert.True(host.Snapshot.Epoch.Value > afterEof.Value);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
            Assert.Equal(output, panel.GetOutputHandle());
            Assert.Equal(panel.Handle, GetParent(output));

            await host.CloseMediaAsync(cancellationToken);
            Assert.Equal(TransportState.Closed, host.Snapshot.Transport);
            Assert.True(IsWindow(output));
            Assert.Equal(20, panel.ParentHandleRecreations);

            var diagnostics = native.CaptureDiagnostics();
            Assert.Contains(diagnostics.Events, e => e.Kind == 3);
        }
        finally
        {
            // Do not release the HWND unless session destruction succeeded.
            // Dispose joins the render worker and releases the native swap
            // chain; only then may the UI destroy the independently owned HWND.
            await host.DisposeAsync();
            nativeDisposed = true;
            if (nativeDisposed)
            {
                panel.ReleaseOutputAfterRuntimeDisposed();
                Assert.False(IsWindow(output));
            }
        }
    }

    private static async Task WaitUntilAsync(
        Func<bool> condition,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (!condition())
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (DateTime.UtcNow >= deadline)
            {
                throw new TimeoutException(
                    "Native WinForms playback did not reach the expected state.");
            }
            await Task.Delay(25, cancellationToken);
        }
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint hwnd);

    [DllImport("user32.dll")]
    private static extern nint GetParent(nint hwnd);
}
