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
    public async Task TemporaryFastForwardUsesActivePlaybackEpoch()
    {
        var calls = new List<string>();
        var host = new DesktopPlaybackHost(
            new FakeRuntime(calls), new FakeSurfaceBinding(calls));
        await host.OpenAndPlayAsync("clip.mkv", (nint)11);
        var expected = host.Snapshot;

        Assert.True(await host.SupportsTemporaryFastForwardAsync(expected));
        await host.SetTemporaryFastForwardAsync(true, expected);
        await host.SetTemporaryFastForwardAsync(false, expected);

        Assert.Equal(new[]
        {
            "open:1", "surface:Win32Window:1", "play:1",
            "fast-forward:True:1", "fast-forward:False:1"
        }, calls);
        await host.SeekAsync(TimeSpan.FromSeconds(2));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.SetTemporaryFastForwardAsync(true, expected));
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
    public async Task DirectoryLocatorFlowsThroughSameActorAndFreshPlaybackEpochs()
    {
        var calls = new List<string>();
        var runtime = new FakeRuntime(calls);
        await using var host = new DesktopPlaybackHost(
            runtime, new FakeSurfaceBinding(calls));
        var disc = new MediaLocator(MediaLocatorKind.Directory, @"C:\clear-disc\BDMV");

        await host.OpenAndPlayAsync(disc, (nint)21);
        Assert.Same(disc, runtime.LastLocator);
        Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
        await host.SeekAsync(TimeSpan.FromSeconds(5));
        await host.OpenAndPlayAsync(disc, (nint)22);
        Assert.Same(disc, runtime.LastLocator);
        Assert.Equal(new[]
        {
            "open:1", "surface:Win32Window:1", "play:1",
            "seek:2:5000000000",
            "open:3", "surface:Win32Window:3", "play:3"
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
        await Assert.ThrowsAsync<ArgumentNullException>(() =>
            host.OpenAndPlayAsync((MediaLocator)null!, (nint)10));
        await Assert.ThrowsAsync<ArgumentException>(() =>
            host.OpenAndPlayAsync(
                new MediaLocator(MediaLocatorKind.Directory, "BDMV"), 0));
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

    [Fact]
    public async Task EmbeddedSubtitleCallsAreBoundToCurrentSessionAndEpoch()
    {
        var calls = new List<string>();
        var runtime = new FakeRuntime(calls);
        await using var host = new DesktopPlaybackHost(
            runtime, new FakeSurfaceBinding(calls));

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.ReadEmbeddedSubtitleTracksAsync(host.Snapshot));

        await host.OpenAndPlayAsync("subtitle-film.mkv", (nint)111);
        var first = host.Snapshot;
        Assert.True(await host.SupportsEmbeddedTextSubtitleCuesAsync(first));
        var inventory = await host.ReadEmbeddedSubtitleTracksAsync(first);
        var track = Assert.Single(inventory);
        Assert.Equal(3U, track.StreamIndex);
        await host.StartEmbeddedTextSubtitleLoadAsync(track.StreamIndex, first);
        var page = await host.ReadEmbeddedTextSubtitleCuesAsync(0, first);
        Assert.Equal("Host cue", Assert.Single(page.Cues).Text);
        Assert.All(runtime.SubtitleContexts, c =>
        {
            Assert.Equal(first.SessionId, c.SessionId);
            Assert.Equal(first.Epoch, c.Epoch);
        });

        await host.SeekAsync(TimeSpan.FromSeconds(12));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.ReadEmbeddedTextSubtitleCuesAsync(0, first));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.StartEmbeddedTextSubtitleLoadAsync(track.StreamIndex, first));
        var second = host.Snapshot;
        await host.StartEmbeddedTextSubtitleLoadAsync(track.StreamIndex, second);
        Assert.Equal(second.Epoch, runtime.SubtitleContexts[^1].Epoch);

        await host.CloseMediaAsync();
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.ReadEmbeddedSubtitleTracksAsync(second));
    }

    [Fact]
    public async Task HostHidesUnsupportedEmbeddedCueExtension()
    {
        var calls = new List<string>();
        var runtime = new FakeRuntime(calls)
        {
            SupportsEmbeddedTextSubtitleCues = false
        };
        await using var host = new DesktopPlaybackHost(
            runtime, new FakeSurfaceBinding(calls));
        await host.OpenAndPlayAsync("film.mkv", (nint)111);
        Assert.False(await host.SupportsEmbeddedTextSubtitleCuesAsync(host.Snapshot));
    }

    [Fact]
    public async Task SeekCannotOvertakeAnActiveSubtitleLoad()
    {
        var calls = new List<string>();
        var runtime = new FakeRuntime(calls)
        {
            WaitForSubtitleStart = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously)
        };
        await using var host = new DesktopPlaybackHost(
            runtime, new FakeSurfaceBinding(calls));
        await host.OpenAndPlayAsync("subtitle-film.mkv", (nint)111);
        var before = host.Snapshot;

        var load = host.StartEmbeddedTextSubtitleLoadAsync(3, before);
        await runtime.SubtitleStartEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var seek = host.SeekAsync(TimeSpan.FromSeconds(2));

        Assert.False(seek.IsCompleted);
        runtime.WaitForSubtitleStart.SetResult();
        await load;
        await seek;
        Assert.Equal(TransportState.Playing, host.Snapshot.Transport);
        Assert.Equal(2UL, host.Snapshot.Epoch.Value);
        Assert.Equal("subtitle-start:1", calls[^2]);
        Assert.Equal("seek:2:2000000000", calls[^1]);
    }

    [Fact]
    public async Task RequestScopedCancelCannotStopNewerSelectionOrCrossSeekEpoch()
    {
        var calls = new List<string>();
        var runtime = new FakeRuntime(calls);
        await using var host = new DesktopPlaybackHost(
            runtime, new FakeSurfaceBinding(calls));
        await host.OpenAndPlayAsync("film.mkv", (nint)111);
        var first = host.Snapshot;
        Assert.True(await host.SupportsEmbeddedTextSubtitleCancellationAsync(first));

        await host.StartEmbeddedTextSubtitleLoadWithIdAsync(3, 501, first);
        await host.StartEmbeddedTextSubtitleLoadWithIdAsync(3, 502, first);
        await host.CancelEmbeddedTextSubtitleLoadAsync(501, first);
        Assert.Equal(502UL, runtime.ActiveSubtitleRequestId);
        await host.CancelEmbeddedTextSubtitleLoadAsync(502, first);
        Assert.Equal(0UL, runtime.ActiveSubtitleRequestId);
        Assert.Equal(new ulong[] { 501, 502 }, runtime.CancelledSubtitleRequests);

        await host.SeekAsync(TimeSpan.FromSeconds(4));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.CancelEmbeddedTextSubtitleLoadAsync(502, first));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.StartEmbeddedTextSubtitleLoadWithIdAsync(3, 503, first));
        var second = host.Snapshot;
        await host.StartEmbeddedTextSubtitleLoadWithIdAsync(3, 503, second);
        Assert.Equal(503UL, runtime.ActiveSubtitleRequestId);
        await host.CloseMediaAsync();
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            host.CancelEmbeddedTextSubtitleLoadAsync(503, second));
    }

    [Fact]
    public async Task HostCanReportLegacyRuntimeWithoutCancellationExtension()
    {
        var calls = new List<string>();
        var runtime = new FakeRuntime(calls)
        {
            SupportsEmbeddedTextSubtitleCancellation = false
        };
        await using var host = new DesktopPlaybackHost(
            runtime, new FakeSurfaceBinding(calls));
        await host.OpenAndPlayAsync("film.mkv", (nint)111);
        Assert.False(await host.SupportsEmbeddedTextSubtitleCancellationAsync(
            host.Snapshot));
        // Legacy Cue ABI v1 still works.
        await host.StartEmbeddedTextSubtitleLoadAsync(3, host.Snapshot);
        Assert.Contains("subtitle-start:1", calls);
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

    private sealed class FakeRuntime(List<string> calls) :
        IPlaybackRuntime, IEmbeddedSubtitleTrackInventoryRuntime,
        IEmbeddedTextSubtitleCueRuntime, IEmbeddedTextSubtitleCancellationRuntime,
        ITemporaryFastForwardRuntime
    {
        public MediaLocator? LastLocator { get; private set; }

        public bool SupportsEmbeddedTextSubtitleCues { get; set; } = true;
        public bool SupportsEmbeddedTextSubtitleCancellation { get; set; } = true;
        public bool SupportsTemporaryFastForward { get; set; } = true;
        public ulong ActiveSubtitleRequestId { get; private set; }
        public List<ulong> CancelledSubtitleRequests { get; } = [];

        public List<RuntimeCommandContext> SubtitleContexts { get; } = [];
        public TaskCompletionSource SubtitleStartEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource? WaitForSubtitleStart { get; set; }

        public ValueTask StartEmbeddedTextSubtitleLoadWithIdAsync(
            uint streamIndex, ulong requestId, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            SubtitleContexts.Add(context);
            Assert.Equal(3U, streamIndex);
            Assert.NotEqual(0UL, requestId);
            ActiveSubtitleRequestId = requestId;
            calls.Add($"subtitle-start-id:{requestId}:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask CancelEmbeddedTextSubtitleLoadAsync(
            ulong requestId, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            SubtitleContexts.Add(context);
            CancelledSubtitleRequests.Add(requestId);
            if (ActiveSubtitleRequestId == requestId)
                ActiveSubtitleRequestId = 0;
            calls.Add($"subtitle-cancel-id:{requestId}:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask<IReadOnlyList<EmbeddedSubtitleTrack>> ReadEmbeddedSubtitleTracksAsync(
            RuntimeCommandContext context, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            SubtitleContexts.Add(context);
            return ValueTask.FromResult<IReadOnlyList<EmbeddedSubtitleTrack>>(
                [new EmbeddedSubtitleTrack(3, EmbeddedSubtitleTrackFormat.Text,
                    "eng", "subrip")]);
        }

        public async ValueTask StartEmbeddedTextSubtitleLoadAsync(
            uint streamIndex, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(3U, streamIndex);
            SubtitleContexts.Add(context);
            calls.Add($"subtitle-start:{context.Epoch.Value}");
            SubtitleStartEntered.TrySetResult();
            if (WaitForSubtitleStart is { } wait)
                await wait.Task.WaitAsync(cancellationToken);
        }

        public ValueTask<EmbeddedTextSubtitleCuePage> ReadEmbeddedTextSubtitleCuesAsync(
            uint startIndex, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            SubtitleContexts.Add(context);
            return ValueTask.FromResult(new EmbeddedTextSubtitleCuePage(
                EmbeddedTextSubtitleLoadState.Ready, 1,
                [new EmbeddedTextSubtitleCue(0, 2_000_000_000, "Host cue")]));
        }

        public ValueTask<RuntimeOpenResult> OpenAsync(
            MediaLocator locator,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            LastLocator = locator;
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

        public ValueTask SetTemporaryFastForwardAsync(
            bool enabled,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            calls.Add($"fast-forward:{enabled}:{context.Epoch.Value}");
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
