using Lapis.Contracts.Capabilities;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Contracts.Time;

namespace Lapis.Application.Runtime;

public readonly record struct RuntimeCommandContext(
    PlaybackSessionId SessionId,
    PlaybackEpoch Epoch);

public sealed record RuntimeOpenResult(
    MediaCapability Capabilities,
    MediaTime? Duration = null);

/// <summary>
/// Application-side port for the native media runtime. Implementations may use
/// P/Invoke, IPC, or another adapter, but those details must not leak upward.
/// </summary>
/// <summary>
/// Optional, read-only chapter lookup for the source-selected title.
/// The playback actor serializes this lookup with its normal epoch-advancing
/// Seek. The adapter must never seek or mutate source navigation itself.
/// </summary>
public interface IChapterNavigationRuntime
{
    ValueTask<MediaTime> ResolveChapterAsync(
        uint chapterId,
        RuntimeCommandContext context,
        CancellationToken cancellationToken);
}

public interface IPlaybackRuntime : IAsyncDisposable
{
    ValueTask<RuntimeOpenResult> OpenAsync(
        MediaLocator locator,
        RuntimeCommandContext context,
        CancellationToken cancellationToken);

    ValueTask PlayAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken);

    ValueTask PauseAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken);

    ValueTask SeekAsync(
        MediaTime position,
        RuntimeCommandContext context,
        CancellationToken cancellationToken);

    ValueTask CloseAsync(
        RuntimeCommandContext context,
        CancellationToken cancellationToken);
}
