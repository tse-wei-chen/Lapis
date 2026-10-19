using Lapis.Application.Runtime;
using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;
using Lapis.Desktop.Hosting;
using Xunit;

namespace Lapis.Desktop.Hosting.Tests;

public sealed class DesktopPlaybackHostTests
{
    [Fact]
    public async Task OpenBindsWindowBeforePlayAndDisposesAfterDetach()
    {
        var calls = new List<string>();
        var runtime = new FakeRuntime(calls);
        var binder = new FakeSurfaceBinding(calls);
        var host = new DesktopPlaybackHost(runtime, binder);

        await host.OpenAndPlayAsync("movie.m2ts", (nint)123);

        Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
        Assert.Equal(new[] { "open:1", "surface:Win32Window:1", "play:1" }, calls);
        Assert.Equal((nint)123, binder.LastSurface.NativeHandle);
        Assert.Equal(host.Snapshot.SessionId, binder.LastContext.SessionId);

        await host.DisposeAsync();
        await host.DisposeAsync();

        Assert.Equal(
            new[] { "open:1", "surface:Win32Window:1", "play:1",
                "surface:None:1", "dispose" },
            calls);
    }

    [Fact]
    public async Task PauseSeekAndWindowReplacementUseCurrentEpoch()
    {
        var calls = new List<string>();
        var host = new DesktopPlaybackHost(
            new FakeRuntime(calls),
            new FakeSurfaceBinding(calls));

        await host.OpenAndPlayAsync("clip.mkv", (nint)11);
        await host.TogglePlayPauseAsync();
        await host.SeekAsync(TimeSpan.FromSeconds(12));
        var bound = await host.ReplaceWindowAsync((nint)22);
        await host.TogglePlayPauseAsync();

        Assert.Equal(VideoOutputSurfaceBindingState.Applied, bound);
        Assert.Equal(new[]
        {
            "open:1", "surface:Win32Window:1", "play:1",
            "pause:1", "seek:2:12000000000",
            "surface:Win32Window:2", "play:2"
        }, calls);

        await host.DisposeAsync();
    }

    [Fact]
    public async Task CloseStopsBeforeDetachingWindow()
    {
        var calls = new List<string>();
        var host = new DesktopPlaybackHost(
            new FakeRuntime(calls), new FakeSurfaceBinding(calls));

        await host.OpenAndPlayAsync("clip.mkv", (nint)123);
        await host.CloseMediaAsync();

        Assert.Equal(TransportState.Closed, host.Snapshot.Transport);
        Assert.Equal(new[]
        {
            "open:1", "surface:Win32Window:1", "play:1",
            "close:2", "surface:None:2"
        }, calls);
        await host.DisposeAsync();
        Assert.Equal("dispose", calls[^1]);
        Assert.Equal(1, calls.Count(call => call == "surface:None:2"));
    }

    [Fact]
    public async Task DetachCompletesBeforeCallerDestroysOldWindowAndRebinds()
    {
        var calls = new List<string>();
        await using var host = new DesktopPlaybackHost(
            new FakeRuntime(calls), new FakeSurfaceBinding(calls));

        await host.OpenAndPlayAsync("clip.mkv", (nint)100);
        var detached = await host.DetachWindowAsync();
        calls.Add("destroy-old-hwnd");
        var rebound = await host.ReplaceWindowAsync((nint)200);

        Assert.Equal(VideoOutputSurfaceBindingState.Deferred, detached);
        Assert.Equal(VideoOutputSurfaceBindingState.Deferred, rebound);
        Assert.Equal(new[]
        {
            "open:1", "surface:Win32Window:1", "play:1",
            "surface:None:1", "destroy-old-hwnd", "surface:Win32Window:1"
        }, calls);
    }

    [Fact]
    public async Task DetachIsIdempotentBeforeOpenOrAfterStop()
    {
        var calls = new List<string>();
        await using var host = new DesktopPlaybackHost(
            new FakeRuntime(calls), new FakeSurfaceBinding(calls));

        await host.DetachWindowAsync();
        await host.OpenAndPlayAsync("clip.mkv", (nint)100);
        await host.CloseMediaAsync();
        await host.DetachWindowAsync();

        Assert.Equal(new[]
        {
            "open:1", "surface:Win32Window:1", "play:1",
            "close:2", "surface:None:2"
        }, calls);
    }

    [Fact]
    public async Task SwitchingAndReopeningMediaUseFreshEpochsAndNoOldWindowLease()
    {
        var calls = new List<string>();
        await using var host = new DesktopPlaybackHost(
            new FakeRuntime(calls), new FakeSurfaceBinding(calls));

        await host.OpenAndPlayAsync("first.mkv", (nint)123);
        await host.SeekAsync(TimeSpan.FromSeconds(3));
        await host.OpenAndPlayAsync("second.mkv", (nint)123);
        await host.CloseMediaAsync();
        await host.OpenAndPlayAsync("second.mkv", (nint)456);

        Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
        Assert.Equal(new[]
        {
            "open:1", "surface:Win32Window:1", "play:1",
            "seek:2:3000000000",
            "open:3", "surface:Win32Window:3", "play:3",
            "close:4", "surface:None:4",
            "open:5", "surface:Win32Window:5", "play:5"
        }, calls);
    }

    [Fact]
    public async Task RejectsInvalidHostInputBeforeCreatingNativeSession()
    {
        var calls = new List<string>();
        await using var host = new DesktopPlaybackHost(
            new FakeRuntime(calls), new FakeSurfaceBinding(calls));

        await Assert.ThrowsAsync<ArgumentException>(() =>
            host.OpenAndPlayAsync("", (nint)123));
        await Assert.ThrowsAsync<ArgumentException>(() =>
            host.OpenAndPlayAsync("movie.mkv", 0));
        await Assert.ThrowsAsync<ArgumentException>(() =>
            host.ReplaceWindowAsync(0));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            host.SeekAsync(TimeSpan.FromSeconds(-1)));

        Assert.Empty(calls);
    }

    [Fact]
    public async Task DisposeStopsNativeEvenWhenSurfaceDetachThrows()
    {
        var calls = new List<string>();
        var binder = new FakeSurfaceBinding(calls) { ThrowOnDetach = true };
        var host = new DesktopPlaybackHost(new FakeRuntime(calls), binder);
        await host.OpenAndPlayAsync("movie.mkv", (nint)123);

        await host.DisposeAsync();

        Assert.NotNull(host.LastShutdownWarning);
        Assert.Equal("dispose", calls[^1]);
        await Assert.ThrowsAsync<ObjectDisposedException>(() =>
            host.TogglePlayPauseAsync());
    }

    private sealed class FakeSurfaceBinding(List<string> calls) : IVideoOutputSurfaceBinding
    {
        public bool ThrowOnDetach { get; set; }
        public VideoOutputSurface LastSurface { get; private set; }
        public RuntimeCommandContext LastContext { get; private set; }

        public ValueTask<VideoOutputSurfaceBindingState> SetVideoOutputSurfaceAsync(
            VideoOutputSurface surface,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            calls.Add($"surface:{surface.Kind}:{context.Epoch.Value}");
            LastSurface = surface;
            LastContext = context;
            if (surface.Kind == VideoOutputSurfaceKind.None && ThrowOnDetach)
            {
                throw new InvalidOperationException("mock detach failed");
            }

            return ValueTask.FromResult(surface.Kind == VideoOutputSurfaceKind.None
                ? VideoOutputSurfaceBindingState.Deferred
                : surface.NativeHandle == (nint)22
                    ? VideoOutputSurfaceBindingState.Applied
                    : VideoOutputSurfaceBindingState.Deferred);
        }
    }

    private sealed class FakeRuntime(List<string> calls) : IPlaybackRuntime
    {
        public ValueTask<RuntimeOpenResult> OpenAsync(
            MediaLocator locator,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(MediaLocatorKind.File, locator.Kind);
            calls.Add($"open:{context.Epoch.Value}");
            return ValueTask.FromResult(new RuntimeOpenResult(MediaCapability.None));
        }

        public ValueTask PlayAsync(
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            calls.Add($"play:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask PauseAsync(
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            calls.Add($"pause:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask SeekAsync(
            MediaTime position,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            calls.Add($"seek:{context.Epoch.Value}:{position.Nanoseconds}");
            return ValueTask.CompletedTask;
        }

        public ValueTask CloseAsync(
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            calls.Add($"close:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask DisposeAsync()
        {
            calls.Add("dispose");
            return ValueTask.CompletedTask;
        }
    }
}
