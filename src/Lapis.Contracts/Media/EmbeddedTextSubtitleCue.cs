namespace Lapis.Contracts.Media;

/// <summary>
/// One owned, plain-text cue from an embedded SubRip/mov_text track.
/// Times use playback media nanoseconds (not wall-clock timestamps).
/// </summary>
public sealed record EmbeddedTextSubtitleCue(
    long StartNanoseconds,
    long EndNanoseconds,
    string Text);

public enum EmbeddedTextSubtitleLoadState
{
    Loading,
    Ready,
    Failed,
    LimitExceeded,
    Cancelled
}

/// <summary>
/// A bounded, immutable page of cues. No native pointers or borrowed buffers
/// cross this contract. Total is the count of cues in a completed timeline.
/// While Loading or on failure, Cues is empty.
/// </summary>
public sealed record EmbeddedTextSubtitleCuePage(
    EmbeddedTextSubtitleLoadState State,
    uint Total,
    IReadOnlyList<EmbeddedTextSubtitleCue> Cues);
