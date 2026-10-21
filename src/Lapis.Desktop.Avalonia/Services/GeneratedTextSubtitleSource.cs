using System.Globalization;
using Lapis.Contracts.Media;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Opt-in adapter for one playback session. No AI service, network access or
/// audio capture is started by discovery or selection. The caller owns the
/// producer and must invalidate it on Seek/Open/Close.
/// </summary>
public sealed class GeneratedTextSubtitleSource : ITextSubtitleSource
{
    public const string Id = "generated-text";
    private readonly string _mediaPath;
    private readonly GeneratedSubtitleTimeline _timeline;
    private readonly TextSubtitleDescriptor _descriptor;
    private readonly Func<bool> _sessionIsCurrent;

    public string SourceId => Id;
    public GeneratedSubtitleTimeline Timeline => _timeline;

    public GeneratedTextSubtitleSource(
        string mediaPath, string displayName, string? languageTag,
        GeneratedSubtitleTimeline timeline, Func<bool> sessionIsCurrent)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(displayName);
        _mediaPath = Path.GetFullPath(mediaPath);
        _timeline = timeline ?? throw new ArgumentNullException(nameof(timeline));
        _sessionIsCurrent = sessionIsCurrent ??
            throw new ArgumentNullException(nameof(sessionIsCurrent));
        _descriptor = new(Id, "session-generated", displayName,
            languageTag, TextSubtitleSourceKind.GeneratedText);
    }

    public Task<IReadOnlyList<TextSubtitleDescriptor>> DiscoverAsync(
        string mediaPath, CancellationToken cancellationToken)
    {
        Validate(mediaPath, cancellationToken);
        return Task.FromResult<IReadOnlyList<TextSubtitleDescriptor>>([_descriptor]);
    }

    public Task<ITextSubtitleTrack> LoadAsync(
        string mediaPath, TextSubtitleDescriptor descriptor,
        CancellationToken cancellationToken)
    {
        Validate(mediaPath, cancellationToken);
        if (descriptor != _descriptor)
            throw new ArgumentException("Unknown generated subtitle track.", nameof(descriptor));
        return Task.FromResult<ITextSubtitleTrack>(_timeline);
    }

    public Task<ITextSubtitleTrack> LoadPreferredAsync(
        string mediaPath, CultureInfo culture, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(culture);
        Validate(mediaPath, cancellationToken);
        // Never activate AI as an implicit Auto source. The catalog's
        // existing sidecar-first Auto policy remains unchanged.
        return Task.FromResult<ITextSubtitleTrack>(SidecarSubtitleTrack.Empty);
    }

    private void Validate(string mediaPath, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        if (!string.Equals(_mediaPath, Path.GetFullPath(mediaPath), comparison))
            throw new ArgumentException("Generated subtitle source belongs to another media file.",
                nameof(mediaPath));
        if (!_sessionIsCurrent())
            throw new InvalidOperationException("Generated subtitle session is no longer active.");
        cancellationToken.ThrowIfCancellationRequested();
    }
}
