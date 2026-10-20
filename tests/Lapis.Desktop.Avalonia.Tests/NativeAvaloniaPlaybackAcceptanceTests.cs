using System.Runtime.InteropServices;
using System.Threading;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Themes.Fluent;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Avalonia.Controls;
using Lapis.Desktop.Avalonia.Views;
using Lapis.Desktop.Avalonia.Services;
using Lapis.Desktop.Avalonia.ViewModels;
using Lapis.Desktop.Hosting;
using Lapis.Runtime.Native;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

/// <summary>
/// Opt-in acceptance, run only by the Native Windows CI job with the actual
/// MediaRuntime DLL and FFmpeg dependencies available. Starts Avalonia's
/// real Win32 desktop backend on an STA, NOT a mocked/headless renderer.
/// </summary>
public sealed class NativeAvaloniaPlaybackAcceptanceTests
{
    [Fact]
    public async Task NativeD3d11SwapChainSurvivesAvaloniaDetachSeekReplayAndClose()
    {
        if (Environment.GetEnvironmentVariable("LAPIS_AVALONIA_NATIVE_ACCEPTANCE") != "1")
            return;

        Assert.True(OperatingSystem.IsWindows());
        var media = Environment.GetEnvironmentVariable("LAPIS_NATIVE_UI_MEDIA");
        Assert.False(string.IsNullOrWhiteSpace(media));
        Assert.True(File.Exists(media));

        var complete = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        AvaloniaAcceptanceApplication.MediaPath = media!;
        AvaloniaAcceptanceApplication.Completed = complete;

        var thread = new Thread(() =>
        {
            try
            {
                AppBuilder.Configure<AvaloniaAcceptanceApplication>()
                    .UsePlatformDetect()
                    .StartWithClassicDesktopLifetime([]);
                if (!complete.Task.IsCompleted)
                    complete.TrySetException(new InvalidOperationException(
                        "Avalonia desktop lifetime exited before the GPU test finished."));
            }
            catch (Exception ex) { complete.TrySetException(ex); }
        })
        {
            IsBackground = true,
            Name = "Lapis.AvaloniaNativeAcceptance.STA"
        };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();

        await complete.Task.WaitAsync(TimeSpan.FromSeconds(75));
        Assert.True(thread.Join(TimeSpan.FromSeconds(10)),
            "Avalonia UI thread failed to exit after Native shutdown.");
    }

    internal static async Task ExerciseAsync(MainWindow window, string media)
    {
        using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        // The new product launches library-first. Acceptance explicitly enters
        // the watch page before exercising the real NativeControlHost.
        var libraryPage = window.FindControl<Grid>("LibraryPage")
            ?? throw new InvalidOperationException("Missing library page.");
        var watchPage = window.FindControl<Grid>("PlayerPage")
            ?? throw new InvalidOperationException("Missing watch page.");
        Assert.True(libraryPage.IsVisible);
        Assert.False(watchPage.IsVisible);
        libraryPage.IsVisible = false;
        watchPage.IsVisible = true;
        var surface = window.FindControl<WindowsVideoSurface>("VideoSurface")
            ?? throw new InvalidOperationException("Avalonia video host is missing.");

        await WaitUntilAsync(() => TryHandle(surface) != 0,
            TimeSpan.FromSeconds(10), cancel.Token);

        var output = surface.OutputHandle;
        Assert.True(IsWindow(output));
        var windowHwnd = window.TryGetPlatformHandle()?.Handle ?? 0;
        Assert.NotEqual(nint.Zero, windowHwnd);
        // The D3D11 child now lives BELOW an Avalonia-managed proxy HWND.
        // Its immediate GetParent need not change during host recycling:
        // Avalonia may move the proxy to its offscreen parking window.
        Assert.NotEqual(nint.Zero, GetParent(output));
        Assert.Equal(windowHwnd, GetAncestor(output, 2));
        // An Avalonia-managed proxy is not the D3D11 render rectangle:
        // the nested output child must receive a real pixel size, not 1x1.
        await WaitUntilAsync(
            () => ClientWidth(output) > 320 && ClientHeight(output) > 200,
            TimeSpan.FromSeconds(5), cancel.Token);
        var originalVideoWidth = ClientWidth(output);
        Assert.Equal(ApartmentState.STA, Thread.CurrentThread.GetApartmentState());

        var runtime = new NativePlaybackRuntime();
        var host = new DesktopPlaybackHost(runtime, runtime);
        try
        {
            await host.OpenAndPlayAsync(media, output, cancel.Token);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);

            // Create the real owned transparent Avalonia top-level above the
            // live D3D11 child. A XAML-only/headless test cannot prove HWND
            // layering even at a basic smoke-test level.
            await using (var overlayPlayer = new PlayerViewModel(
                new NativePlayerPlaybackService(() => surface.OutputHandle)))
            {
                var chrome = new WatchChromeWindow(window, overlayPlayer, [],
                    () => Task.CompletedTask, () => { });
                try
                {
                    var overlayHwnd = chrome.TryGetPlatformHandle()?.Handle ?? 0;
                    Assert.True(chrome.IsVisible);
                    Assert.NotEqual(nint.Zero, overlayHwnd);
                    Assert.NotEqual(windowHwnd, overlayHwnd);
                    Assert.NotEqual(output, overlayHwnd);
                    Assert.True(IsWindow(overlayHwnd));
                    Assert.True(IsWindow(output));
                    chrome.SetSubtitleTrack(SidecarSubtitleTrack.Parse(
                        "1\n00:00:00,000 --> 00:00:02,000\n字幕測試\n"));
                    await Task.Delay(180, cancel.Token);
                    Assert.True(IsWindow(output));
                }
                finally
                {
                    chrome.Close();
                }
            }
            await WaitUntilAsync(
                () => runtime.CaptureDiagnostics().Events.Any(e => e.Kind == 3),
                TimeSpan.FromSeconds(12), cancel.Token); // SwapChainCreated

            // Exercise Avalonia's actual NativeControlHost detach/re-attach.
            // The output HWND is a separate resource that D3D11 continues to
            // borrow; detaching the Avalonia visual must PARK, not destroy it.
            // The immersive Watch surface sits directly in a Grid rather
            // than the old framed Border; test HWND lifecycle, not a
            // particular choice of presentation container.
            var container = Assert.IsAssignableFrom<Panel>(surface.Parent);
            var initialEpoch = host.Snapshot.Epoch;
            for (var i = 0; i < 6; i++)
            {
                container.Children.Remove(surface);
                await WaitUntilAsync(
                    () => IsWindow(output) &&
                          GetAncestor(output, 2) != windowHwnd,
                    TimeSpan.FromSeconds(3), cancel.Token);
                Assert.Equal(output, surface.OutputHandle);

                // Leave time for Avalonia's deferred proxy destruction on
                // alternating cycles; other cycles exercise immediate reuse.
                if ((i & 1) == 0)
                    await Task.Delay(85, cancel.Token);

                container.Children.Add(surface);
                await WaitUntilAsync(
                    () => IsWindow(output) &&
                          GetAncestor(output, 2) == windowHwnd,
                    TimeSpan.FromSeconds(3), cancel.Token);
                Assert.Equal(output, surface.OutputHandle);
                Assert.NotEqual(TransportState.Faulted, host.Snapshot.Transport);
                window.Width = 1090 + i * 9;
                window.Height = 720 + i * 7;
                await Task.Delay(20, cancel.Token);
            }

            // A hosted CI desktop may clamp the top-level window to its
            // virtual display bounds. Force an independent Avalonia layout
            // change on the NativeControlHost itself, and verify that the
            // D3D11 child tracks that DIP-to-physical-pixel resize.
            surface.Width = 520;
            surface.Height = 290;
            await WaitUntilAsync(
                () => ClientWidth(output) != originalVideoWidth &&
                      ClientWidth(output) > 320 &&
                      ClientHeight(output) > 200,
                TimeSpan.FromSeconds(4), cancel.Token);
            await host.SeekAsync(TimeSpan.FromMilliseconds(250), cancel.Token);
            Assert.True(host.Snapshot.Epoch.Value > initialEpoch.Value);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);

            await WaitUntilAsync(() => host.Snapshot.Transport is
                TransportState.Ended or TransportState.Faulted,
                TimeSpan.FromSeconds(15), cancel.Token);
            Assert.Equal(TransportState.Ended, host.Snapshot.Transport);

            var afterEof = host.Snapshot.Epoch;
            await host.OpenAndPlayAsync(media, output, cancel.Token);
            Assert.True(host.Snapshot.Epoch.Value > afterEof.Value);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);

            await host.CloseMediaAsync(cancel.Token);
            Assert.Equal(TransportState.Closed, host.Snapshot.Transport);
            Assert.True(IsWindow(output));
            Assert.Contains(runtime.CaptureDiagnostics().Events, e => e.Kind == 3);
        }
        finally
        {
            // The renderer ACK/worker joins must finish asynchronously before
            // UI ownership of the child HWND is relinquished.
            await host.DisposeAsync();
            surface.ReleaseAfterPlaybackStopped();
            Assert.False(IsWindow(output));
        }
    }

    private static nint TryHandle(WindowsVideoSurface surface)
    {
        try { return surface.OutputHandle; }
        catch (InvalidOperationException) { return 0; }
    }

    private static async Task WaitUntilAsync(Func<bool> predicate,
        TimeSpan timeout, CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (!predicate())
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (DateTime.UtcNow >= deadline)
                throw new TimeoutException(
                    "Avalonia Native HWND/GPU lifecycle did not reach expected state.");
            await Task.Delay(25, cancellationToken);
        }
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint handle);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    private static int ClientWidth(nint handle) =>
        GetClientRect(handle, out var rect) ? rect.Right - rect.Left : 0;

    private static int ClientHeight(nint handle) =>
        GetClientRect(handle, out var rect) ? rect.Bottom - rect.Top : 0;

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(nint handle, out NativeRect rect);

    [DllImport("user32.dll")]
    private static extern nint GetParent(nint handle);

    [DllImport("user32.dll")]
    private static extern nint GetAncestor(nint handle, uint flags);
}

public sealed class AvaloniaAcceptanceApplication : global::Avalonia.Application
{
    internal static TaskCompletionSource? Completed { get; set; }
    internal static string? MediaPath { get; set; }

    public override void Initialize() => Styles.Add(new FluentTheme());

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var window = new MainWindow();
            desktop.MainWindow = window;
            window.Opened += async (_, _) =>
            {
                try
                {
                    await NativeAvaloniaPlaybackAcceptanceTests.ExerciseAsync(
                        window, MediaPath ??
                        throw new InvalidOperationException("No media fixture."));
                    Completed?.TrySetResult();
                }
                catch (Exception ex)
                {
                    Completed?.TrySetException(ex);
                }
                finally
                {
                    window.Close();
                }
            };
        }
        base.OnFrameworkInitializationCompleted();
    }
}
