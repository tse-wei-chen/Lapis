using System.Globalization;
using Lapis.Contracts.Media;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Per-session text-subtitle registry. Watch UI deals only in descriptors;
/// file access and text decoding stay inside their respective providers.
/// </summary>
public sealed class TextSubtitleSourceCatalog
{
    private readonly ITextSubtitleSource[] _sources;
    private readonly Dictionary<string, ITextSubtitleSource> _byId;

    public static TextSubtitleSourceCatalog Default { get; } =
        // SRT remains the Auto source for backward compatibility. WebVTT
        // appears alongside it as an explicit selectable text track.
        new([new SidecarSrtTextSubtitleSource(),
             new SidecarWebVttTextSubtitleSource()]);

    public TextSubtitleSourceCatalog(IEnumerable<ITextSubtitleSource> sources)
    {
        ArgumentNullException.ThrowIfNull(sources);
        _sources = sources.ToArray();
        if (_sources.Length == 0)
            throw new ArgumentException("At least one text subtitle source is required.", nameof(sources));
        _byId = new Dictionary<string, ITextSubtitleSource>(StringComparer.Ordinal);
        foreach (var source in _sources)
        {
            if (source is null || string.IsNullOrWhiteSpace(source.SourceId) ||
                !_byId.TryAdd(source.SourceId, source))
                throw new ArgumentException("Subtitle source IDs must be unique and non-empty.", nameof(sources));
        }
    }

    public async Task<IReadOnlyList<TextSubtitleDescriptor>> DiscoverAsync(
        string mediaPath, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        var found = new List<TextSubtitleDescriptor>(64);
        var seen = new HashSet<(string SourceId, string TrackId)>();
        foreach (var source in _sources)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var tracks = await source.DiscoverAsync(mediaPath, cancellationToken);
            // A provider may return an empty result just after cancellation.
            cancellationToken.ThrowIfCancellationRequested();
            foreach (var track in tracks)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (track.SourceId != source.SourceId || string.IsNullOrWhiteSpace(track.TrackId) ||
                    string.IsNullOrWhiteSpace(track.DisplayName))
                    throw new InvalidOperationException(
                        $"Invalid subtitle track returned by {source.SourceId}.");
                if (seen.Add((track.SourceId, track.TrackId)))
                    found.Add(track);
                if (found.Count == 64) return found.ToArray();
            }
        }
        return found.ToArray();
    }

    public Task<ITextSubtitleTrack> LoadAsync(
        string mediaPath, TextSubtitleDescriptor descriptor,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        ArgumentNullException.ThrowIfNull(descriptor);
        cancellationToken.ThrowIfCancellationRequested();
        if (!_byId.TryGetValue(descriptor.SourceId, out var source))
            throw new ArgumentException("Subtitle track source is not registered.", nameof(descriptor));
        return source.LoadAsync(mediaPath, descriptor, cancellationToken);
    }

    public Task<ITextSubtitleTrack> LoadPreferredAsync(
        string mediaPath, CultureInfo culture, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        ArgumentNullException.ThrowIfNull(culture);
        cancellationToken.ThrowIfCancellationRequested();
        return _sources[0].LoadPreferredAsync(mediaPath, culture, cancellationToken);
    }
}

/// <summary>
/// Local unprotected SRT implementation. Its file paths never escape the
/// provider into Watch business logic; other providers may use stream IDs.
/// </summary>
public sealed class SidecarSrtTextSubtitleSource : ITextSubtitleSource
{
    public const string Id = "local-srt";
    public string SourceId => Id;

    public async Task<IReadOnlyList<TextSubtitleDescriptor>> DiscoverAsync(
        string mediaPath, CancellationToken cancellationToken)
    {
        var files = await SidecarSubtitleTrack.DiscoverForFileAsync(
            mediaPath, cancellationToken);
        var baseName = Path.GetFileNameWithoutExtension(mediaPath) + ".";
        return files.Select(file =>
        {
            var name = file.DisplayName;
            string? language = null;
            if (name.StartsWith(baseName, StringComparison.OrdinalIgnoreCase) &&
                name.Length > baseName.Length + 4)
                language = name.Substring(baseName.Length,
                    name.Length - baseName.Length - 4);

            return new TextSubtitleDescriptor(
                SourceId, file.FilePath, name, language,
                TextSubtitleSourceKind.LocalSidecar);
        }).ToArray();
    }

    public async Task<ITextSubtitleTrack> LoadAsync(
        string mediaPath, TextSubtitleDescriptor descriptor,
        CancellationToken cancellationToken)
    {
        if (descriptor.SourceId != Id || descriptor.Kind != TextSubtitleSourceKind.LocalSidecar)
            throw new ArgumentException("Track does not belong to the SRT source.", nameof(descriptor));

        // Do not let a fabricated or stale descriptor read an unrelated path.
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        var fullMediaPath = Path.GetFullPath(mediaPath);
        var fullSubtitlePath = Path.GetFullPath(descriptor.TrackId);
        if (!string.Equals(Path.GetDirectoryName(fullMediaPath),
                Path.GetDirectoryName(fullSubtitlePath), comparison))
            throw new ArgumentException("Subtitle must be next to its media file.", nameof(descriptor));

        var basename = Path.GetFileNameWithoutExtension(fullMediaPath);
        var name = Path.GetFileName(fullSubtitlePath);
        var tagged = name.StartsWith(basename + ".", comparison) &&
                     name.EndsWith(".srt", StringComparison.OrdinalIgnoreCase) &&
                     name.Length > basename.Length + 5;
        if (tagged)
        {
            var tag = name.Substring(basename.Length + 1,
                name.Length - basename.Length - 5);
            tagged = tag.Length is >= 2 and <= 35 &&
                     tag.All(c => char.IsAsciiLetterOrDigit(c) || c == '-');
        }
        if (!name.Equals(basename + ".srt", comparison) && !tagged)
            throw new ArgumentException("Subtitle name does not match the media file.", nameof(descriptor));

        return await SidecarSubtitleTrack.LoadExactFileAsync(
            fullSubtitlePath, cancellationToken);
    }

    public async Task<ITextSubtitleTrack> LoadPreferredAsync(
        string mediaPath, CultureInfo culture, CancellationToken cancellationToken) =>
        await SidecarSubtitleTrack.LoadForFileAsync(mediaPath, culture, cancellationToken);
}
