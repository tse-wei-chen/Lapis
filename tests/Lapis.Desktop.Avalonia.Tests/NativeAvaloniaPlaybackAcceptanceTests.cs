using System.Runtime.InteropServices;
using System.Threading;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using FluentAvalonia.Styling;
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
        var bigPicture = window.FindControl<BigPictureLibraryView>("BigPictureLibrary")
            ?? throw new InvalidOperationException("Big Picture view missing.");
        Assert.True(bigPicture.IsVisible);
        bigPicture.HandleGamepadAction(GamepadAction.NextCategory);
        bigPicture.HandleGamepadAction(GamepadAction.PreviousCategory);
        bigPicture.HandleGamepadAction(GamepadAction.Back);
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
            await using             var playbackService = new AcceptancePlaybackService(host);
            var overlayPlayer = new PlayerViewModel(playbackService);
            await overlayPlayer.OpenAsync(
                new Lapis.Contracts.Media.MediaLocator(
                    Lapis.Contracts.Media.MediaLocatorKind.File, media));
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
                var subtitle = chrome.FindControl<TextBlock>("WatchSubtitleText");
                var subtitleHost = chrome.FindControl<Border>("WatchSubtitleHost");
                Assert.NotNull(subtitle);
                Assert.NotNull(subtitleHost);
                Assert.Equal("字幕測試", subtitle!.Text);
                Assert.True(subtitleHost!.IsVisible);
                var controls = chrome.FindControl<Border>("WatchControls");
                Assert.NotNull(controls);
                Assert.True(controls!.IsVisible);
                chrome.NotifyPointerActivity();
                Assert.True(controls.IsVisible);

                window.Activate();
                await WaitUntilAsync(
                    () => window.IsActive,
                    TimeSpan.FromSeconds(2), cancel.Token);
                var timeline = chrome.FindControl<Slider>("WatchTimeline");
                var backSeek = chrome.FindControl<Button>("WatchBackSeek");
                var forwardSeek = chrome.FindControl<Button>("WatchForwardSeek");
                Assert.NotNull(timeline);
                Assert.NotNull(backSeek);
                Assert.NotNull(forwardSeek);

                var seekDuration = host.Snapshot.Duration?.ToTimeSpan() ??
                    throw new InvalidOperationException(
                        "The native playback snapshot has no seekable duration.");
                Assert.True(seekDuration > TimeSpan.Zero,
                    $"Native media has no usable duration ({seekDuration}).");

                // CI uses a compact churn asset. A 10-second skip/30-second
                // seek cannot be exercised on a clip shorter than 40 seconds.
                // Keep these real pointer/keyboard transport tests mandatory
                // whenever a long-form fixture is supplied.
                if (seekDuration > TimeSpan.FromSeconds(40))
                {
                await overlayPlayer.SeekToAsync(TimeSpan.FromSeconds(30));
                await Task.Delay(TimeSpan.FromMilliseconds(500), cancel.Token);
                await WaitUntilAsync(
                    () => overlayPlayer.CanSeek,
                    TimeSpan.FromSeconds(5), cancel.Token);
                Assert.InRange(
                    Math.Abs((host.Snapshot.Position.ToTimeSpan() -
                        TimeSpan.FromSeconds(30)).TotalSeconds),
                    0, 1);

                var beforeBackPosition = host.Snapshot.Position.ToTimeSpan();
                var beforeBackClick = host.Snapshot.Epoch;
                await ClickControlAsync(
                    window, chrome, backSeek!, 0.5, TimeSpan.FromMilliseconds(80),
                    cancel.Token);
                await WaitUntilAsync(
                    () => host.Snapshot.Epoch.Value > beforeBackClick.Value,
                    TimeSpan.FromSeconds(5), cancel.Token);
                await WaitUntilAsync(
                    () => overlayPlayer.CanSeek,
                    TimeSpan.FromSeconds(5), cancel.Token);
                var afterBackPosition = host.Snapshot.Position.ToTimeSpan();
                var backAdvanceSeconds =
                    (beforeBackPosition - afterBackPosition).TotalSeconds;
                Assert.True(
                    backAdvanceSeconds is >= 9 and <= 11,
                    $"A real back-button click should subtract 10 seconds " +
                    $"(before={beforeBackPosition} after={afterBackPosition} " +
                    $"epoch={host.Snapshot.Epoch.Value} " +
                    $"status={overlayPlayer.StatusText}; " +
                    $"advance={backAdvanceSeconds:F3}s " +
                    $"requested={playbackService.LastSeekPosition}).");
                Assert.NotNull(playbackService.LastSeekPosition);
                Assert.InRange(
                    Math.Abs((playbackService.LastSeekPosition!.Value -
                        (beforeBackPosition - TimeSpan.FromSeconds(10))).TotalSeconds),
                    0, 1);
                Assert.InRange(
                    Math.Abs(overlayPlayer.SeekPositionSeconds -
                        afterBackPosition.TotalSeconds),
                    0, 1);

                var beforeForwardPosition = host.Snapshot.Position.ToTimeSpan();
                var beforeForwardClick = host.Snapshot.Epoch;
                await ClickControlAsync(
                    window, chrome, forwardSeek!, 0.5, TimeSpan.FromMilliseconds(80),
                    cancel.Token);
                try
                {
                    await WaitUntilAsync(
                        () => host.Snapshot.Epoch.Value > beforeForwardClick.Value,
                        TimeSpan.FromSeconds(5), cancel.Token);
                }
                catch (TimeoutException exception)
                {
                    throw new TimeoutException(
                        $"Forward click did not advance playback. " +
                        $"before={beforeForwardClick.Value} " +
                        $"after={host.Snapshot.Epoch.Value} " +
                        $"position={host.Snapshot.Position.ToTimeSpan()} " +
                        $"transport={host.Snapshot.Transport} " +
                        $"canSeek={overlayPlayer.CanSeek} " +
                        $"status={overlayPlayer.StatusText}",
                        exception);
                }

                await WaitUntilAsync(
                    () => overlayPlayer.CanSeek,
                    TimeSpan.FromSeconds(5), cancel.Token);
                var afterForwardPosition = host.Snapshot.Position.ToTimeSpan();
                var forwardAdvanceSeconds =
                    (afterForwardPosition - beforeForwardPosition).TotalSeconds;
                Assert.True(
                    forwardAdvanceSeconds is >= 9 and <= 11,
                    $"A real forward-button click should add 10 seconds " +
                    $"(before={beforeForwardPosition} after={afterForwardPosition} " +
                    $"epoch={host.Snapshot.Epoch.Value} " +
                    $"status={overlayPlayer.StatusText}; " +
                    $"advance={forwardAdvanceSeconds:F3}s).");
                Assert.NotNull(playbackService.LastSeekPosition);
                Assert.InRange(
                    Math.Abs((playbackService.LastSeekPosition!.Value -
                        (beforeForwardPosition + TimeSpan.FromSeconds(10))).TotalSeconds),
                    0, 1);
                Assert.InRange(
                    Math.Abs(overlayPlayer.SeekPositionSeconds -
                        afterForwardPosition.TotalSeconds),
                    0, 1);

                var beforeFastForwardPosition =
                    host.Snapshot.Position.ToTimeSpan();
                var fastForwardStarted = DateTime.UtcNow;
                await ClickControlAsync(
                    window, chrome, forwardSeek!, 0.5, TimeSpan.FromMilliseconds(1300),
                    cancel.Token);
                Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
                await WaitUntilAsync(
                    () => overlayPlayer.CanSeek,
                    TimeSpan.FromSeconds(5), cancel.Token);
                var fastForwardElapsed = DateTime.UtcNow - fastForwardStarted;
                var fastForwardAdvance =
                    (host.Snapshot.Position.ToTimeSpan() -
                        beforeFastForwardPosition).TotalSeconds;
                Assert.True(
                    fastForwardAdvance > fastForwardElapsed.TotalSeconds,
                    $"Holding fast-forward should advance faster than normal " +
                    $"(advanced={fastForwardAdvance:F3}s elapsed=" +
                    $"{fastForwardElapsed.TotalSeconds:F3}s).");

                var beforeTimelineClick = host.Snapshot.Epoch;
                await ClickControlAsync(
                    window, chrome, timeline!, 0.65, TimeSpan.FromMilliseconds(80),
                    cancel.Token);
                await WaitUntilAsync(
                    () => host.Snapshot.Epoch.Value > beforeTimelineClick.Value,
                    TimeSpan.FromSeconds(5), cancel.Token);
                await WaitUntilAsync(
                    () => overlayPlayer.CanSeek,
                    TimeSpan.FromSeconds(5), cancel.Token);
                var timelinePosition = host.Snapshot.Position.ToTimeSpan();
                Assert.InRange(
                    timelinePosition.TotalSeconds / seekDuration.TotalSeconds,
                    0.60, 0.70);
                Assert.InRange(
                    Math.Abs(overlayPlayer.SeekPositionSeconds -
                        timelinePosition.TotalSeconds),
                    0, 1);
                }

                var otherWindow = new Window { Width = 100, Height = 100 };
                try
                {
                    otherWindow.Show();
                    otherWindow.Activate();
                    await WaitUntilAsync(
                        () => otherWindow.IsActive && !IsTopmost(
                            chrome.TryGetPlatformHandle()?.Handle ?? 0),
                        TimeSpan.FromSeconds(2), cancel.Token);
                }
                finally
                {
                    otherWindow.Close();
                }
                window.Activate();
                await WaitUntilAsync(
                    () => window.IsActive && IsTopmost(
                        chrome.TryGetPlatformHandle()?.Handle ?? 0),
                    TimeSpan.FromSeconds(2), cancel.Token);
            }
            finally
            {
                chrome.Close();
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
            // Never seek past the end of an intentionally tiny CI fixture.
            var shortSeek = TimeSpan.FromTicks(Math.Min(
                (host.Snapshot.Duration?.ToTimeSpan() ?? TimeSpan.Zero).Ticks / 2,
                TimeSpan.FromMilliseconds(250).Ticks));
            await overlayPlayer.SeekToAsync(shortSeek);
            Assert.True(host.Snapshot.Epoch.Value > initialEpoch.Value);
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);

            var duration = host.Snapshot.Duration?.ToTimeSpan() ?? TimeSpan.Zero;
            if (duration > TimeSpan.FromMinutes(1))
                Assert.True(duration >= TimeSpan.FromHours(2));
            var seekPositions = duration > TimeSpan.FromMinutes(1)
                ? new[]
                {
                    TimeSpan.FromMilliseconds(750),
                    TimeSpan.FromMinutes(10),
                    TimeSpan.FromTicks(duration.Ticks / 2),
                    duration - TimeSpan.FromSeconds(10)
                }
                : new[]
                {
                    TimeSpan.FromTicks(Math.Min(duration.Ticks / 3,
                        TimeSpan.FromMilliseconds(750).Ticks)),
                    TimeSpan.FromTicks(Math.Min(duration.Ticks * 2 / 3,
                        TimeSpan.FromSeconds(2).Ticks))
                };
            foreach (var position in seekPositions)
            {
                overlayPlayer.BeginScrub();
                overlayPlayer.SeekPositionSeconds = position.TotalSeconds;
                await overlayPlayer.CommitScrubAsync();
                Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
                Assert.InRange(
                    Math.Abs((host.Snapshot.Position.ToTimeSpan() - position).TotalSeconds),
                    0, 2);
            }

            // Absolute ±10-second actions require a fixture longer than
            // the action itself; they are covered above for longer media.
            if (duration > TimeSpan.FromSeconds(30))
            {
            var beforeForward = host.Snapshot.Epoch;
            overlayPlayer.ForwardCommand.Execute(null);
            await WaitUntilAsync(
                () => host.Snapshot.Epoch.Value > beforeForward.Value,
                TimeSpan.FromSeconds(5), cancel.Token);
            await WaitUntilAsync(
                () => overlayPlayer.CanSeek,
                TimeSpan.FromSeconds(5), cancel.Token);
            var beforeBackward = host.Snapshot.Epoch;
            overlayPlayer.BackCommand.Execute(null);
            await WaitUntilAsync(
                () => host.Snapshot.Epoch.Value > beforeBackward.Value,
                TimeSpan.FromSeconds(5), cancel.Token);
            Assert.True(await overlayPlayer.SetTemporaryFastForwardAsync(true));
            Assert.True(await overlayPlayer.SetTemporaryFastForwardAsync(false));
            Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
            }

            if (duration <= TimeSpan.FromMinutes(1))
            {
                await WaitUntilAsync(() => host.Snapshot.Transport is
                    TransportState.Ended or TransportState.Faulted,
                    TimeSpan.FromSeconds(15), cancel.Token);
                Assert.Equal(TransportState.Ended, host.Snapshot.Transport);

                var afterEof = host.Snapshot.Epoch;
                await host.OpenAndPlayAsync(media, output, cancel.Token);
                Assert.True(host.Snapshot.Epoch.Value > afterEof.Value);
                Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
            }

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

    private static async Task ClickControlAsync(
        Window owner, Window overlay, Control control, double horizontalFraction,
        TimeSpan hold, CancellationToken cancellationToken)
    {
        var ownerHandle = owner.TryGetPlatformHandle()?.Handle ?? 0;
        Assert.NotEqual(nint.Zero, ownerHandle);
        owner.Activate();
        await WaitUntilAsync(
            () => owner.IsActive &&
                (GetWindowLongPtr(
                    overlay.TryGetPlatformHandle()?.Handle ?? 0, -20)
                    .ToInt64() & 0x00000008) != 0,
            TimeSpan.FromSeconds(2), cancellationToken);

        var point = control.PointToScreen(new Point(
            control.Bounds.Width * horizontalFraction,
            control.Bounds.Height / 2));
        var overlayHandle = overlay.TryGetPlatformHandle()?.Handle ?? 0;
        Assert.True((GetWindowLongPtr(overlayHandle, -20).ToInt64() & 0x00000008) != 0,
            $"The watch overlay must be topmost while its owner is active " +
            $"(ownerActive={owner.IsActive}, exStyle=0x{GetWindowLongPtr(overlayHandle, -20):X}).");
        Assert.True(SetCursorPos((int)point.X, (int)point.Y));
        var hit = WindowFromPoint(new NativePoint((int)point.X, (int)point.Y));
        Assert.NotEqual(nint.Zero, hit);
        var expectedRoot = overlayHandle;
        var hitRoot = GetAncestor(hit, 2);
        Assert.True(hitRoot == expectedRoot,
            $"Mouse target did not resolve to the overlay. " +
            $"point=({point.X},{point.Y}) control={control.Name} " +
            $"overlay=0x{expectedRoot:X} hit=0x{hit:X} root=0x{hitRoot:X} " +
            $"overlayRect={WindowRectText(expectedRoot)}");
        await WaitUntilAsync(
            () => control.IsPointerOver,
            TimeSpan.FromSeconds(2), cancellationToken);
        MouseEvent(MouseLeftDown, 0, 0, 0, 0);
        await Task.Delay(hold, cancellationToken);
        MouseEvent(MouseLeftUp, 0, 0, 0, 0);
        await Task.Delay(100, cancellationToken);
    }

    private sealed class AcceptancePlaybackService(DesktopPlaybackHost host)
        : IPlayerPlaybackService, ITemporaryFastForwardPlaybackService
    {
        public TimeSpan? LastSeekPosition { get; private set; }

        public PlaybackSnapshot Snapshot => host.Snapshot;

        public Task OpenAsync(
            Lapis.Contracts.Media.MediaLocator locator,
            CancellationToken cancellationToken) => Task.CompletedTask;

        public Task ToggleAsync(CancellationToken cancellationToken) =>
            host.TogglePlayPauseAsync(cancellationToken);

        public Task SeekAsync(TimeSpan position, CancellationToken cancellationToken)
        {
            LastSeekPosition = position;
            return host.SeekAsync(position, cancellationToken);
        }

        public Task CloseAsync(CancellationToken cancellationToken) =>
            host.CloseMediaAsync(cancellationToken);

        public IAsyncEnumerable<PlaybackSnapshot> ObserveSnapshotsAsync(
            CancellationToken cancellationToken) =>
            host.ObserveSnapshotsAsync(cancellationToken);

        public Task<bool> SupportsTemporaryFastForwardAsync(
            PlaybackSnapshot expected, CancellationToken cancellationToken) =>
            host.SupportsTemporaryFastForwardAsync(expected, cancellationToken);

        public Task SetTemporaryFastForwardAsync(
            bool enabled, PlaybackSnapshot expected,
            CancellationToken cancellationToken) =>
            host.SetTemporaryFastForwardAsync(
                enabled, expected, cancellationToken);

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(nint handle);

    private const uint MouseLeftDown = 0x0002;
    private const uint MouseLeftUp = 0x0004;

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll", EntryPoint = "mouse_event")]
    private static extern void MouseEvent(
        uint flags, uint dx, uint dy, uint data, nuint extraInfo);

    [DllImport("user32.dll")]
    private static extern nint WindowFromPoint(NativePoint point);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtr(nint handle, int index);

    private static string WindowRectText(nint handle) =>
        GetWindowRect(handle, out var rect)
            ? $"({rect.Left},{rect.Top},{rect.Right},{rect.Bottom})"
            : "unavailable";

    private static bool IsTopmost(nint handle) =>
        handle != 0 &&
        (GetWindowLongPtr(handle, -20).ToInt64() & 0x00000008) != 0;

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct NativePoint(int x, int y)
    {
        public readonly int X = x;
        public readonly int Y = y;
    }

    private static int ClientWidth(nint handle) =>
        GetClientRect(handle, out var rect) ? rect.Right - rect.Left : 0;

    private static int ClientHeight(nint handle) =>
        GetClientRect(handle, out var rect) ? rect.Bottom - rect.Top : 0;

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(nint handle, out NativeRect rect);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(nint handle, out NativeRect rect);

    [DllImport("user32.dll")]
    private static extern nint GetParent(nint handle);

    [DllImport("user32.dll")]
    private static extern nint GetAncestor(nint handle, uint flags);
}

public sealed class AvaloniaAcceptanceApplication : global::Avalonia.Application
{
    internal static TaskCompletionSource? Completed { get; set; }
    internal static string? MediaPath { get; set; }

    public override void Initialize()
    {
        RequestedThemeVariant = global::Avalonia.Styling.ThemeVariant.Dark;
        Styles.Add(new FluentAvaloniaTheme());
    }

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
