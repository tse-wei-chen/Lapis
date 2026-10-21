using System.Threading.Channels;
using Lapis.Application.Playback;
using Lapis.Application.Runtime;
using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;
using Xunit;

namespace Lapis.Application.Tests;

public sealed class PlaybackSessionTests
{
    [Fact]
    public async Task CommandsAreSerializedAndSeekAdvancesEpoch()
    {
        var runtime = new FakeRuntime();
        await using var session = new PlaybackSession(runtime, new PlaybackSessionId(Guid.Parse("11111111-1111-1111-1111-111111111111")));

        await session.ExecuteAsync(new OpenMediaCommand(new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        await session.ExecuteAsync(new PlayCommand());
        await session.ExecuteAsync(new SeekCommand(MediaTime.FromSeconds(5)));

        Assert.Equal(TransportState.Playing, session.Snapshot.Transport);
        Assert.Equal(2UL, session.Snapshot.Epoch.Value);
        Assert.Equal(MediaTime.FromSeconds(5), session.Snapshot.Position);
        Assert.Equal(new[] { "open:1", "play:1", "seek:2" }, runtime.Calls);
    }

    [Theory]
    [InlineData(60)]
    [InlineData(90)]
    public async Task SeekAtOrAfterDurationUsesDecodablePositionWithoutFault(long requestedSeconds)
    {
        var runtime = new DurationSeekRuntime();
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        await session.ExecuteAsync(new PlayCommand());

        await session.ExecuteAsync(new SeekCommand(MediaTime.FromSeconds(requestedSeconds)));

        var safeEnd = MediaTime.FromMilliseconds(59_750);
        Assert.Equal(safeEnd, runtime.LastSeekPosition);
        Assert.Equal(safeEnd, session.Snapshot.Position);
        Assert.Equal(TransportState.Playing, session.Snapshot.Transport);
        Assert.Equal(new PlaybackEpoch(2), session.Snapshot.Epoch);
        Assert.Null(session.Snapshot.LastError);
    }

    [Fact]
    public async Task NegativeSeekIsRejectedWithoutFaultingPlayback()
    {
        var runtime = new DurationSeekRuntime();
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            session.ExecuteAsync(new SeekCommand(new MediaTime(-1))).AsTask());
        Assert.Equal(TransportState.Ready, session.Snapshot.Transport);
        Assert.Equal(new PlaybackEpoch(1), session.Snapshot.Epoch);
        Assert.Null(runtime.LastSeekPosition);
        Assert.Null(session.Snapshot.LastError);
    }

    [Fact]
    public async Task FailedNativeSeekPublishesFaultInsteadOfLeavingSeekingState()
    {
        var runtime = new DurationSeekRuntime { FailSeek = true };
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        await session.ExecuteAsync(new PlayCommand());

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            session.ExecuteAsync(new SeekCommand(MediaTime.FromSeconds(30))).AsTask());

        Assert.Equal(TransportState.Faulted, session.Snapshot.Transport);
        Assert.Equal(new PlaybackEpoch(2), session.Snapshot.Epoch);
        Assert.Equal("SeekFailed", session.Snapshot.LastError?.Code);
    }

    [Fact]
    public async Task ChapterSeekIsSerializedAndUsesTheExistingEpochAdvancingSeek()
    {
        var runtime = new ChapterRuntime();
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(
            new OpenMediaCommand(new MediaLocator(MediaLocatorKind.Directory, "BDMV")));
        await session.ExecuteAsync(new PlayCommand());

        await session.ExecuteAsync(new SeekChapterCommand(7));
        Assert.Equal(TransportState.Playing, session.Snapshot.Transport);
        Assert.Equal(2UL, session.Snapshot.Epoch.Value);
        Assert.Equal(MediaTime.FromSeconds(12), session.Snapshot.Position);
        Assert.Equal(new[]
        {
            "open:1", "play:1", "chapter:1:7", "seek:2"
        }, runtime.Calls);

        await session.ExecuteAsync(new PauseCommand());
        await session.ExecuteAsync(new SeekChapterCommand(7));
        Assert.Equal(TransportState.Paused, session.Snapshot.Transport);
        Assert.Equal(3UL, session.Snapshot.Epoch.Value);
        Assert.Equal(new[]
        {
            "open:1", "play:1", "chapter:1:7", "seek:2",
            "pause:2", "chapter:2:7", "seek:3"
        }, runtime.Calls);
    }

    [Fact]
    public async Task UnsupportedAndUnknownChapterDoNotAdvanceEpochOrFaultPlayback()
    {
        await using var simple = new PlaybackSession(new FakeRuntime());
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            simple.ExecuteAsync(new SeekChapterCommand(7)).AsTask());
        await simple.ExecuteAsync(
            new OpenMediaCommand(new MediaLocator(MediaLocatorKind.File, "clip.mkv")));
        await Assert.ThrowsAsync<NotSupportedException>(() =>
            simple.ExecuteAsync(new SeekChapterCommand(7)).AsTask());
        Assert.Equal(TransportState.Ready, simple.Snapshot.Transport);
        Assert.Equal(1UL, simple.Snapshot.Epoch.Value);

        var runtime = new ChapterRuntime { RejectChapter = true };
        await using var disc = new PlaybackSession(runtime);
        await disc.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.Directory, "BDMV")));
        await disc.ExecuteAsync(new PlayCommand());
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            disc.ExecuteAsync(new SeekChapterCommand(8)).AsTask());
        Assert.Equal(TransportState.Playing, disc.Snapshot.Transport);
        Assert.Equal(1UL, disc.Snapshot.Epoch.Value);
        Assert.Equal(new[] { "open:1", "play:1", "chapter:1:8" }, runtime.Calls);
    }

    [Fact]
    public async Task CallerCancellationAfterStartDoesNotInterruptRuntimeMutation()
    {
        var runtime = new ControlledRuntime();
        await using var session = new PlaybackSession(runtime);
        using var cancellation = new CancellationTokenSource();

        var openTask = session.ExecuteAsync(
            new OpenMediaCommand(new MediaLocator(MediaLocatorKind.File, "movie.mkv")),
            cancellation.Token).AsTask();

        await runtime.OpenStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => openTask);

        Assert.False(runtime.OpenRuntimeCancellation.IsCancellationRequested);
        runtime.ReleaseOpen.TrySetResult();

        // The next command can only execute after the open command has completed,
        // proving caller cancellation did not strand the session in Opening.
        await session.ExecuteAsync(new PlayCommand());
        Assert.Equal(TransportState.Playing, session.Snapshot.Transport);
        Assert.Equal(new[] { "open:1", "play:1" }, runtime.Calls);
    }

    [Fact]
    public async Task CancellationWhileQueuedPreventsCommandExecution()
    {
        var runtime = new ControlledRuntime();
        await using var session = new PlaybackSession(runtime);

        var openTask = session.ExecuteAsync(
            new OpenMediaCommand(new MediaLocator(MediaLocatorKind.File, "movie.mkv"))).AsTask();
        await runtime.OpenStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var cancellation = new CancellationTokenSource();
        var closeTask = session.ExecuteAsync(new CloseCommand(), cancellation.Token).AsTask();
        await Task.Yield();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => closeTask);

        runtime.ReleaseOpen.TrySetResult();
        await openTask;
        await session.ExecuteAsync(new PlayCommand());

        Assert.DoesNotContain(runtime.Calls, call => call.StartsWith("close:", StringComparison.Ordinal));
        Assert.Equal(TransportState.Playing, session.Snapshot.Transport);
    }

    [Fact]
    public async Task DisposeCancelsActiveRuntimeAndRejectsNewCommands()
    {
        var runtime = new ControlledRuntime { BlockOpenUntilShutdown = true };
        var session = new PlaybackSession(runtime);

        var openTask = session.ExecuteAsync(
            new OpenMediaCommand(new MediaLocator(MediaLocatorKind.File, "movie.mkv"))).AsTask();
        await runtime.OpenStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var disposeTask = session.DisposeAsync().AsTask();
        await runtime.OpenCancelledByShutdown.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await disposeTask;

        await Assert.ThrowsAsync<ObjectDisposedException>(() => openTask);
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => session.ExecuteAsync(new PlayCommand()).AsTask());
        Assert.Equal(1, runtime.DisposeCount);
        Assert.Equal(TransportState.Closed, session.Snapshot.Transport);
    }

    [Fact]
    public async Task ConcurrentDisposeCallsShareOneShutdown()
    {
        var runtime = new FakeRuntime();
        var session = new PlaybackSession(runtime);

        var first = session.DisposeAsync().AsTask();
        var second = session.DisposeAsync().AsTask();
        await Task.WhenAll(first, second);

        Assert.Equal(1, runtime.DisposeCount);
    }

    [Fact]
    public async Task NativeObservationsUpdatePositionAndPublishNaturalEnd()
    {
        var runtime = new ObservingRuntime
        {
            Duration = MediaTime.FromSeconds(42)
        };
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        Assert.Equal(MediaTime.FromSeconds(42), session.Snapshot.Duration);
        await session.ExecuteAsync(new PlayCommand());

        await runtime.PollStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        runtime.Emit(new RuntimePlaybackObservation(
            session.SessionId, new PlaybackEpoch(1),
            MediaTime.FromSeconds(3), TransportState.Playing));
        await WaitForSnapshotAsync(session, snapshot =>
            snapshot.Position == MediaTime.FromSeconds(3));

        runtime.Emit(new RuntimePlaybackObservation(
            session.SessionId, new PlaybackEpoch(1),
            MediaTime.FromSeconds(42), TransportState.Ended));
        await WaitForSnapshotAsync(session, snapshot =>
            snapshot.Transport == TransportState.Ended);
        Assert.Equal(MediaTime.FromSeconds(42), session.Snapshot.Position);
        Assert.Null(session.Snapshot.LastError);

        await session.ExecuteAsync(new CloseCommand());
        Assert.Equal(TransportState.Closed, session.Snapshot.Transport);
        Assert.Null(session.Snapshot.Duration);
    }

    [Fact]
    public async Task ObservationFromBeforeSeekCannotOverwriteNewEpoch()
    {
        var runtime = new ObservingRuntime();
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        await session.ExecuteAsync(new PlayCommand());
        await runtime.PollStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await session.ExecuteAsync(new SeekCommand(MediaTime.FromSeconds(20)));
        runtime.Emit(new RuntimePlaybackObservation(
            session.SessionId, new PlaybackEpoch(1),
            MediaTime.FromSeconds(2), TransportState.Ended));
        await runtime.WaitForDeliveredReadsAsync(1);
        Assert.Equal(new PlaybackEpoch(2), session.Snapshot.Epoch);
        Assert.Equal(TransportState.Playing, session.Snapshot.Transport);
        Assert.Equal(MediaTime.FromSeconds(20), session.Snapshot.Position);

        runtime.Emit(new RuntimePlaybackObservation(
            session.SessionId, new PlaybackEpoch(2),
            MediaTime.FromSeconds(21), TransportState.Playing));
        await WaitForSnapshotAsync(session, snapshot =>
            snapshot.Position == MediaTime.FromSeconds(21));
    }

    [Fact]
    public async Task SameEpochStalePlayingReadCannotUndoPause()
    {
        var runtime = new ObservingRuntime();
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        await session.ExecuteAsync(new PlayCommand());
        await runtime.PollStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await session.ExecuteAsync(new PauseCommand());
        runtime.Emit(new RuntimePlaybackObservation(
            session.SessionId, new PlaybackEpoch(1),
            MediaTime.FromSeconds(10), TransportState.Playing));
        await runtime.WaitForDeliveredReadsAsync(1);
        Assert.Equal(TransportState.Paused, session.Snapshot.Transport);
        Assert.Equal(MediaTime.Zero, session.Snapshot.Position);
    }

    [Fact]
    public async Task ObservationFaultReconcilesWithoutChangingEpoch()
    {
        var runtime = new ObservingRuntime();
        await using var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        await session.ExecuteAsync(new PlayCommand());
        await runtime.PollStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        runtime.Emit(new RuntimePlaybackObservation(
            session.SessionId, new PlaybackEpoch(1),
            MediaTime.FromSeconds(5), TransportState.Faulted));
        await WaitForSnapshotAsync(session, snapshot =>
            snapshot.Transport == TransportState.Faulted);
        Assert.Equal("NativeFaulted", session.Snapshot.LastError?.Code);
        Assert.Equal(new PlaybackEpoch(1), session.Snapshot.Epoch);
    }

    [Fact]
    public async Task ObservationDisposalCancelsPendingReadBeforeRuntimeDispose()
    {
        var runtime = new ObservingRuntime();
        var session = new PlaybackSession(runtime);
        await session.ExecuteAsync(new OpenMediaCommand(
            new MediaLocator(MediaLocatorKind.File, "movie.mkv")));
        await session.ExecuteAsync(new PlayCommand());
        await runtime.PollStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await session.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, runtime.DisposeCount);
        Assert.True(runtime.ReadCanceled);
    }

    private static async Task WaitForSnapshotAsync(
        PlaybackSession session, Func<PlaybackSnapshot, bool> predicate)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        while (!predicate(session.Snapshot))
        {
            await Task.Delay(10, timeout.Token);
        }
    }

    private sealed class ObservingRuntime : FakeRuntime, IPlaybackRuntimeObserver
    {
        private readonly Channel<RuntimePlaybackObservation> _updates =
            Channel.CreateUnbounded<RuntimePlaybackObservation>();
        private int _readsDelivered;
        public TaskCompletionSource PollStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool ReadCanceled { get; private set; }
        public MediaTime? Duration { get; init; }

        public override ValueTask<RuntimeOpenResult> OpenAsync(
            MediaLocator locator, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            _ = base.OpenAsync(locator, context, cancellationToken);
            return ValueTask.FromResult(
                new RuntimeOpenResult(MediaCapability.Chapters, Duration));
        }

        public async ValueTask<RuntimePlaybackObservation> ReadSnapshotAsync(
            PlaybackSessionId sessionId, CancellationToken cancellationToken)
        {
            PollStarted.TrySetResult();
            try
            {
                var value = await _updates.Reader.ReadAsync(cancellationToken);
                Interlocked.Increment(ref _readsDelivered);
                return value;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                ReadCanceled = true;
                throw;
            }
        }

        public void Emit(RuntimePlaybackObservation observation) =>
            _updates.Writer.TryWrite(observation);

        public async Task WaitForDeliveredReadsAsync(int count)
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            while (Volatile.Read(ref _readsDelivered) < count)
            {
                await Task.Delay(10, timeout.Token);
            }
            // The poller already received this value; let the actor consume the
            // serialized observation envelope before asserting its state.
            await Task.Delay(30);
        }
    }

    private class FakeRuntime : IPlaybackRuntime
    {
        public List<string> Calls { get; } = [];
        public int DisposeCount { get; private set; }

        public virtual ValueTask<RuntimeOpenResult> OpenAsync(
            MediaLocator locator,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            Calls.Add($"open:{context.Epoch.Value}");
            return ValueTask.FromResult(new RuntimeOpenResult(MediaCapability.Chapters));
        }

        public virtual ValueTask PlayAsync(RuntimeCommandContext context, CancellationToken cancellationToken)
        {
            Calls.Add($"play:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public virtual ValueTask PauseAsync(RuntimeCommandContext context, CancellationToken cancellationToken)
        {
            Calls.Add($"pause:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public virtual ValueTask SeekAsync(
            MediaTime position,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            Calls.Add($"seek:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public virtual ValueTask CloseAsync(RuntimeCommandContext context, CancellationToken cancellationToken)
        {
            Calls.Add($"close:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public virtual ValueTask DisposeAsync()
        {
            DisposeCount++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class DurationSeekRuntime : FakeRuntime
    {
        public bool FailSeek { get; init; }
        public MediaTime? LastSeekPosition { get; private set; }

        public override ValueTask<RuntimeOpenResult> OpenAsync(
            MediaLocator locator, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            _ = base.OpenAsync(locator, context, cancellationToken);
            return ValueTask.FromResult(new RuntimeOpenResult(
                MediaCapability.Chapters, MediaTime.FromSeconds(60)));
        }

        public override ValueTask SeekAsync(
            MediaTime position, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            LastSeekPosition = position;
            if (FailSeek) throw new InvalidOperationException("Injected native seek failure.");
            return base.SeekAsync(position, context, cancellationToken);
        }
    }

    private sealed class ChapterRuntime : FakeRuntime, IChapterNavigationRuntime
    {
        public bool RejectChapter { get; init; }

        public ValueTask<MediaTime> ResolveChapterAsync(
            uint chapterId, RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            Calls.Add($"chapter:{context.Epoch.Value}:{chapterId}");
            if (RejectChapter)
            {
                throw new InvalidOperationException("Chapter does not exist.");
            }
            return ValueTask.FromResult(MediaTime.FromSeconds(12));
        }
    }

    private sealed class ControlledRuntime : FakeRuntime
    {
        public TaskCompletionSource OpenStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseOpen { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource OpenCancelledByShutdown { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public CancellationToken OpenRuntimeCancellation { get; private set; }
        public bool BlockOpenUntilShutdown { get; init; }

        public override async ValueTask<RuntimeOpenResult> OpenAsync(
            MediaLocator locator,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            Calls.Add($"open:{context.Epoch.Value}");
            OpenRuntimeCancellation = cancellationToken;
            OpenStarted.TrySetResult();

            try
            {
                if (BlockOpenUntilShutdown)
                {
                    await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken).ConfigureAwait(false);
                }
                else
                {
                    await ReleaseOpen.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                OpenCancelledByShutdown.TrySetResult();
                throw;
            }

            return new RuntimeOpenResult(MediaCapability.Chapters);
        }
    }
}
