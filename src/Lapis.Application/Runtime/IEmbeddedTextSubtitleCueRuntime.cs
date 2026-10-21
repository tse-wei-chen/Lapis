using Lapis.Contracts.Media;

namespace Lapis.Application.Runtime;

/// <summary>
/// Optional, per-session embedded SubRip/mov_text timeline consumer.
/// The decoder runs in a separate native source/worker and never takes
/// packets from live A/V playback. Each call is scoped to the exact Epoch.
/// Implementations must support cancellation, including after native returns.
/// </summary>
public interface IEmbeddedTextSubtitleCueRuntime
{
    /// <summary>
    /// Start or replace a text-only subtitle load. May retire a previous
    /// worker; callers must not block the UI thread while joining it.
    /// </summary>
    ValueTask StartEmbeddedTextSubtitleLoadAsync(
        uint streamIndex,
        RuntimeCommandContext context,
        CancellationToken cancellationToken);

    /// <summary>
    /// Poll a completed immutable timeline in pages (up to 16 cues).
    /// Loading returns zero cues. Seek/Open/Close retire the generation.
    /// </summary>
    ValueTask<EmbeddedTextSubtitleCuePage> ReadEmbeddedTextSubtitleCuesAsync(
        uint startIndex,
        RuntimeCommandContext context,
        CancellationToken cancellationToken);
}
