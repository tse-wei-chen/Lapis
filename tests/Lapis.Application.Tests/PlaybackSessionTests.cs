using Lapis.Application.Graph;
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
    public void GraphPlanRejectsUnavailableCapabilities()
    {
        var builder = new GraphBuilder()
            .AddNode(new GraphNode("source", GraphNodeKind.Source))
            .AddNode(new GraphNode("output", GraphNodeKind.AudioSink, MediaCapability.BitstreamOutput))
            .Connect("source", "output");

        var exception = Assert.Throws<InvalidOperationException>(
            () => builder.Build(MediaCapability.None));

        Assert.Contains(nameof(MediaCapability.BitstreamOutput), exception.Message, StringComparison.Ordinal);
    }

    private sealed class FakeRuntime : IPlaybackRuntime
    {
        public List<string> Calls { get; } = [];

        public ValueTask<RuntimeOpenResult> OpenAsync(
            MediaLocator locator,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            Calls.Add($"open:{context.Epoch.Value}");
            return ValueTask.FromResult(new RuntimeOpenResult(MediaCapability.Chapters));
        }

        public ValueTask PlayAsync(RuntimeCommandContext context, CancellationToken cancellationToken)
        {
            Calls.Add($"play:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask PauseAsync(RuntimeCommandContext context, CancellationToken cancellationToken)
        {
            Calls.Add($"pause:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask SeekAsync(
            MediaTime position,
            RuntimeCommandContext context,
            CancellationToken cancellationToken)
        {
            Calls.Add($"seek:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask CloseAsync(RuntimeCommandContext context, CancellationToken cancellationToken)
        {
            Calls.Add($"close:{context.Epoch.Value}");
            return ValueTask.CompletedTask;
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
