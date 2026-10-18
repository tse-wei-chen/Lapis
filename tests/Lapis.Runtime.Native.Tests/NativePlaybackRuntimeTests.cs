using System.Runtime.InteropServices;
using Lapis.Application.Runtime;
using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;
using Lapis.Runtime.Native.Interop;
using Xunit;

namespace Lapis.Runtime.Native.Tests;

public sealed class NativePlaybackRuntimeTests
{
    [Fact]
    public void ConstructorRejectsAbiMismatchBeforeCreatingRuntime()
    {
        var api = new FakeNativeApi { AbiVersion = 99 };

        var exception = Assert.Throws<NotSupportedException>(
            () => new NativePlaybackRuntime(api));

        Assert.Contains("ABI 99", exception.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("runtime_create", api.Calls);
    }

    [Fact]
    public async Task CommandsMapContractsAndDisposeSessionBeforeRuntime()
    {
        var api = new FakeNativeApi
        {
            OpenResult = new NativeOpenResult
            {
                Capabilities = (ulong)(MediaCapability.Chapters | MediaCapability.HdrOutput),
                Duration = new NativeMediaTime { Nanoseconds = 9_500_000_000 },
                HasDuration = 1
            }
        };
        var sessionId = new PlaybackSessionId(
            Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"));
        var runtime = new NativePlaybackRuntime(api);

        var opened = await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.m2ts"),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);

        Assert.Equal(
            MediaCapability.Chapters | MediaCapability.HdrOutput,
            opened.Capabilities);
        Assert.Equal(new MediaTime(9_500_000_000), opened.Duration);
        Assert.Equal(sessionId.Value.ToByteArray(), api.LastSessionIdBytes);
        Assert.Equal(NativeMediaLocatorKind.File, api.LastLocatorKind);
        Assert.Equal("movie.m2ts", api.LastLocatorValue);
        Assert.Equal((ulong)1, api.LastEpoch.Value);
        Assert.Equal(1, api.SessionCreateCount);

        await runtime.PlayAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);
        await runtime.PauseAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);
        await runtime.SeekAsync(
            new MediaTime(3_000_000_000),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(2)),
            CancellationToken.None);
        Assert.Equal(3_000_000_000, api.LastPosition.Nanoseconds);
        Assert.Equal((ulong)2, api.LastEpoch.Value);

        await runtime.CloseAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(3)),
            CancellationToken.None);
        Assert.Equal((ulong)3, api.LastEpoch.Value);

        await runtime.DisposeAsync();
        await runtime.DisposeAsync();

        Assert.Equal(1, api.SessionDestroyCount);
        Assert.Equal(1, api.RuntimeDestroyCount);
        Assert.Equal(
            new[] { "session_destroy", "runtime_destroy" },
            api.Calls.TakeLast(2));
    }

    [Fact]
    public async Task RuntimeBindsToExactlyOnePlaybackSessionId()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var first = new PlaybackSessionId(Guid.NewGuid());
        var second = new PlaybackSessionId(Guid.NewGuid());

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "first.mkv"),
            new RuntimeCommandContext(first, new PlaybackEpoch(1)),
            CancellationToken.None);

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.PlayAsync(
                new RuntimeCommandContext(second, new PlaybackEpoch(1)),
                CancellationToken.None));

        Assert.Equal(1, api.SessionCreateCount);
    }

    [Fact]
    public async Task NativeStateAndEpochFailuresBecomeInvalidOperationExceptions()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var sessionId = new PlaybackSessionId(Guid.NewGuid());

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.PlayResult = NativeResult.StaleEpoch;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.PlayAsync(
                new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
                CancellationToken.None));

        api.PlayResult = NativeResult.InvalidState;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.PlayAsync(
                new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
                CancellationToken.None));
    }

    [Fact]
    public async Task CancellationBeforeInteropDoesNotInvokeNativeCommand()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var context = new RuntimeCommandContext(
            new PlaybackSessionId(Guid.NewGuid()),
            new PlaybackEpoch(1));
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await runtime.PlayAsync(context, cancellation.Token));

        Assert.Equal(0, api.SessionCreateCount);
        Assert.DoesNotContain("session_play", api.Calls);
    }

    [Fact]
    public void VideoOutputSurfaceAbiUsesNativePointerAlignment()
    {
        Assert.Equal(2u, NativeAbi.Version);
        Assert.Equal(typeof(int), Enum.GetUnderlyingType(typeof(NativeVideoOutputSurfaceKind)));
        Assert.Equal(IntPtr.Size == 8 ? 8L : 4L,
            Marshal.OffsetOf<NativeVideoOutputSurface>(
                nameof(NativeVideoOutputSurface.NativeHandle)).ToInt64());
        Assert.Equal(IntPtr.Size == 8 ? 16 : 8,
            Marshal.SizeOf<NativeVideoOutputSurface>());
    }

    [Fact]
    public async Task OutputBindingUsesOwnPortPreservesEpochAndSupportsDetach()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        IVideoOutputSurfaceBinding bindingPort = runtime;
        var sessionId = new PlaybackSessionId(Guid.NewGuid());
        var initial = new RuntimeCommandContext(sessionId, new PlaybackEpoch(0));
        var hwnd = (nint)0x123456;
        var state = await bindingPort.SetVideoOutputSurfaceAsync(
            VideoOutputSurface.ForWin32Window(hwnd),
            initial,
            CancellationToken.None);

        Assert.Equal(VideoOutputSurfaceBindingState.Deferred, state);
        Assert.Equal(NativeVideoOutputSurfaceKind.Win32Window, api.LastSurface.Kind);
        Assert.Equal((nuint)hwnd, api.LastSurface.NativeHandle);
        Assert.Equal((ulong)0, api.LastEpoch.Value);
        Assert.Equal(1, api.SessionCreateCount);
        Assert.Contains("session_set_video_output_surface", api.Calls);

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.SurfaceBinding = NativeVideoOutputSurfaceBinding.Applied;
        state = await runtime.ClearVideoOutputSurfaceAsync(
            new RuntimeCommandContext(sessionId, new PlaybackEpoch(1)),
            CancellationToken.None);
        Assert.Equal(VideoOutputSurfaceBindingState.Applied, state);
        Assert.Equal(NativeVideoOutputSurfaceKind.None, api.LastSurface.Kind);
        Assert.Equal((nuint)0, api.LastSurface.NativeHandle);
        Assert.Equal((ulong)1, api.LastEpoch.Value);
        Assert.Equal(1, api.SessionCreateCount);
    }

    [Fact]
    public async Task OutputBindingValidatesBeforeSessionCreationAndMapsFailures()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var sessionId = new PlaybackSessionId(Guid.NewGuid());
        var context = new RuntimeCommandContext(sessionId, new PlaybackEpoch(0));

        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                new VideoOutputSurface(VideoOutputSurfaceKind.Win32Window, 0),
                context, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                new VideoOutputSurface(VideoOutputSurfaceKind.None, (nint)1),
                context, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                new VideoOutputSurface((VideoOutputSurfaceKind)99, (nint)1),
                context, CancellationToken.None));
        Assert.Equal(0, api.SessionCreateCount);

        api.SurfaceResult = NativeResult.StaleEpoch;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
        api.SurfaceResult = NativeResult.Unsupported;
        await Assert.ThrowsAsync<NotSupportedException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
        api.SurfaceResult = NativeResult.OutputUnavailable;
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
        api.SurfaceResult = NativeResult.Ok;
        api.SurfaceBinding = (NativeVideoOutputSurfaceBinding)99;
        await Assert.ThrowsAsync<NativeMediaRuntimeException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(VideoOutputSurface.None,
                context, CancellationToken.None));
    }

    [Fact]
    public async Task OutputBindingCancellationDoesNotEnterNative()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var context = new RuntimeCommandContext(
            new PlaybackSessionId(Guid.NewGuid()), new PlaybackEpoch(0));
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await runtime.SetVideoOutputSurfaceAsync(
                VideoOutputSurface.ForWin32Window((nint)111),
                context, cancellation.Token));
        Assert.Equal(0, api.SessionCreateCount);
        Assert.DoesNotContain("session_set_video_output_surface", api.Calls);
    }

    [Fact]
    public void NativeSnapshotLayoutMirrorsCAbi()
    {
        Assert.Equal(40, Marshal.SizeOf<NativePlaybackSnapshot>());
        Assert.Equal(16L,
            Marshal.OffsetOf<NativePlaybackSnapshot>(
                nameof(NativePlaybackSnapshot.Epoch)).ToInt64());
        Assert.Equal(24L,
            Marshal.OffsetOf<NativePlaybackSnapshot>(
                nameof(NativePlaybackSnapshot.Position)).ToInt64());
        Assert.Equal(32L,
            Marshal.OffsetOf<NativePlaybackSnapshot>(
                nameof(NativePlaybackSnapshot.Transport)).ToInt64());
    }

    [Fact]
    public async Task ObservationUsesExistingSessionAndPreservesNativeEpochAndPosition()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        IPlaybackRuntimeObserver observer = runtime;
        var id = new PlaybackSessionId(
            Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await observer.ReadSnapshotAsync(id, CancellationToken.None));
        Assert.Equal(0, api.SessionCreateCount);

        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(id, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.Snapshot = new NativePlaybackSnapshot
        {
            SessionId = NativePlaybackSessionId.From(id),
            Epoch = new NativePlaybackEpoch { Value = 1 },
            Position = new NativeMediaTime { Nanoseconds = 1_250_000_000 },
            Transport = NativeTransportState.Ended
        };
        var observed = await observer.ReadSnapshotAsync(id, CancellationToken.None);
        Assert.Equal(id, observed.SessionId);
        Assert.Equal(new PlaybackEpoch(1), observed.Epoch);
        Assert.Equal(new MediaTime(1_250_000_000), observed.Position);
        Assert.Equal(TransportState.Ended, observed.Transport);
        Assert.Equal(1, api.SessionCreateCount);
        Assert.Contains("session_get_snapshot", api.Calls);

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await observer.ReadSnapshotAsync(
                new PlaybackSessionId(Guid.NewGuid()), CancellationToken.None));
    }

    [Fact]
    public async Task ObservationRejectsInvalidNativePayloadAndCancellation()
    {
        var api = new FakeNativeApi();
        await using var runtime = new NativePlaybackRuntime(api);
        var id = new PlaybackSessionId(Guid.NewGuid());
        await runtime.OpenAsync(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv"),
            new RuntimeCommandContext(id, new PlaybackEpoch(1)),
            CancellationToken.None);

        api.Snapshot = new NativePlaybackSnapshot
        {
            SessionId = NativePlaybackSessionId.From(id),
            Epoch = new NativePlaybackEpoch { Value = 1 },
            Position = new NativeMediaTime { Nanoseconds = -1 },
            Transport = NativeTransportState.Playing
        };
        await Assert.ThrowsAsync<NativeMediaRuntimeException>(async () =>
            await runtime.ReadSnapshotAsync(id, CancellationToken.None));

        var malformed = api.Snapshot;
        malformed.Position.Nanoseconds = 5;
        malformed.Transport = (NativeTransportState)99;
        api.Snapshot = malformed;
        await Assert.ThrowsAsync<NativeMediaRuntimeException>(async () =>
            await runtime.ReadSnapshotAsync(id, CancellationToken.None));

        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        var calls = api.Calls.Count(x => x == "session_get_snapshot");
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await runtime.ReadSnapshotAsync(id, canceled.Token));
        Assert.Equal(calls, api.Calls.Count(x => x == "session_get_snapshot"));
    }

    private sealed class FakeNativeApi : INativeMediaRuntimeApi
    {
        internal uint AbiVersion { get; set; } = NativeAbi.Version;
        internal NativeOpenResult OpenResult { get; set; }
        internal NativePlaybackSnapshot Snapshot { get; set; }
        internal NativeResult PlayResult { get; set; } = NativeResult.Ok;
        internal NativeResult SurfaceResult { get; set; } = NativeResult.Ok;
        internal NativeVideoOutputSurfaceBinding SurfaceBinding { get; set; } = NativeVideoOutputSurfaceBinding.Deferred;
        internal NativeVideoOutputSurface LastSurface { get; private set; }
        internal List<string> Calls { get; } = new();
        internal int SessionCreateCount { get; private set; }
        internal int SessionDestroyCount { get; private set; }
        internal int RuntimeDestroyCount { get; private set; }
        internal byte[]? LastSessionIdBytes { get; private set; }
        internal NativeMediaLocatorKind LastLocatorKind { get; private set; }
        internal string? LastLocatorValue { get; private set; }
        internal NativePlaybackEpoch LastEpoch { get; private set; }
        internal NativeMediaTime LastPosition { get; private set; }

        public uint GetAbiVersion() => AbiVersion;

        public NativeResult RuntimeCreate(out nint runtime)
        {
            Calls.Add("runtime_create");
            runtime = 11;
            return NativeResult.Ok;
        }

        public void RuntimeDestroy(nint runtime)
        {
            Assert.Equal((nint)11, runtime);
            Calls.Add("runtime_destroy");
            RuntimeDestroyCount++;
        }

        public NativeResult SessionCreate(
            nint runtime,
            NativePlaybackSessionId sessionId,
            out nint session)
        {
            Assert.Equal((nint)11, runtime);
            LastSessionIdBytes = sessionId.ToByteArray();
            Assert.Contains(LastSessionIdBytes, static value => value != 0);
            Calls.Add("session_create");
            SessionCreateCount++;
            session = 22;
            return NativeResult.Ok;
        }

        public void SessionDestroy(nint session)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_destroy");
            SessionDestroyCount++;
        }

        public NativeResult SessionOpen(
            nint session,
            NativeMediaLocatorKind kind,
            string value,
            NativePlaybackEpoch epoch,
            out NativeOpenResult result)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_open");
            LastLocatorKind = kind;
            LastLocatorValue = value;
            LastEpoch = epoch;
            result = OpenResult;
            return NativeResult.Ok;
        }

        public NativeResult SessionPlay(nint session, NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_play");
            LastEpoch = epoch;
            return PlayResult;
        }

        public NativeResult SessionPause(nint session, NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_pause");
            LastEpoch = epoch;
            return NativeResult.Ok;
        }

        public NativeResult SessionSeek(
            nint session,
            NativeMediaTime position,
            NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_seek");
            LastPosition = position;
            LastEpoch = epoch;
            return NativeResult.Ok;
        }

        public NativeResult SessionSetVideoOutputSurface(
            nint session,
            NativeVideoOutputSurface surface,
            NativePlaybackEpoch epoch,
            out NativeVideoOutputSurfaceBinding binding)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_set_video_output_surface");
            LastSurface = surface;
            LastEpoch = epoch;
            binding = SurfaceBinding;
            return SurfaceResult;
        }

        public NativeResult SessionGetSnapshot(
            nint session, out NativePlaybackSnapshot snapshot)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_get_snapshot");
            snapshot = Snapshot;
            return NativeResult.Ok;
        }

        public NativeResult SessionClose(nint session, NativePlaybackEpoch epoch)
        {
            Assert.Equal((nint)22, session);
            Calls.Add("session_close");
            LastEpoch = epoch;
            return NativeResult.Ok;
        }
    }
}
