using System.Globalization;

namespace Lapis.Contracts.Media;

/// <summary>
/// Text-only subtitle timeline. This contract must remain free of UI and
/// decoder dependencies; bitmap PGS/Blu-ray overlays use a distinct interface.
/// </summary>
public interface ITextSubtitleTrack
{
    string? At(TimeSpan position);
}

public enum TextSubtitleSourceKind
{
    LocalSidecar,
    EmbeddedText,
    GeneratedText
}

/// <summary>
/// Source-qualified, immutable identity. TrackId is opaque and interpreted
/// only by its owning source, not by the Watch UI.
/// </summary>
public sealed record TextSubtitleDescriptor(
    string SourceId,
    string TrackId,
    string DisplayName,
    string? LanguageTag,
    TextSubtitleSourceKind Kind);

/// <summary>
/// An asynchronous text-subtitle source scoped to a media item. Implementations
/// may use local SRT files, embedded subtitle packets or translated text.
/// Discovery and load must honor cancellation, including seek/close invalidation.
/// </summary>
public interface ITextSubtitleSource
{
    string SourceId { get; }

    Task<IReadOnlyList<TextSubtitleDescriptor>> DiscoverAsync(
        string mediaPath, CancellationToken cancellationToken);

    Task<ITextSubtitleTrack> LoadAsync(
        string mediaPath, TextSubtitleDescriptor descriptor,
        CancellationToken cancellationToken);

    Task<ITextSubtitleTrack> LoadPreferredAsync(
        string mediaPath, CultureInfo culture, CancellationToken cancellationToken);
}
