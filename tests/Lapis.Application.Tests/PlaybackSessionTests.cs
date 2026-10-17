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
