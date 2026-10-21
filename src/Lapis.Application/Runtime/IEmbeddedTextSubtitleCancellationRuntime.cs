namespace Lapis.Application.Runtime;

/// <summary>
/// Optional request-identity-scoped cancellation for embedded text decoding.
/// Separate from Cue API v1, so compatible older runtimes keep working.
/// The logical request ID must be unique and nonzero for each selection.
/// </summary>
public interface IEmbeddedTextSubtitleCancellationRuntime
{
    bool SupportsEmbeddedTextSubtitleCancellation { get; }

    ValueTask StartEmbeddedTextSubtitleLoadWithIdAsync(
        uint streamIndex, ulong requestId, RuntimeCommandContext context,
        CancellationToken cancellationToken);

    /// <summary>
    /// Cooperative stop request (not a blocking decoder join). A late cancel
    /// of an obsolete request ID must never retire a newer selection.
    /// </summary>
    ValueTask CancelEmbeddedTextSubtitleLoadAsync(
        ulong requestId, RuntimeCommandContext context,
        CancellationToken cancellationToken);
}
