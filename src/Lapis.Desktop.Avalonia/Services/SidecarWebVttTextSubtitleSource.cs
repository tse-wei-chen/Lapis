using System.Globalization;
using System.Net;
using System.Text.RegularExpressions;
using Lapis.Contracts.Media;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Local, unprotected WebVTT timed-text source. Supports cue identifiers,
/// timestamps, cue settings and common inline formatting; it does not implement
/// WebVTT layout, CSS, voice/ruby styling or live streaming.
/// </summary>
public sealed class SidecarWebVttTextSubtitleSource : ITextSubtitleSource
{
    public const string Id = "local-webvtt";
    public string SourceId => Id;

    private static readonly Regex CueTiming = new(
        @"^(?<start>(?:\d{2,}:)?\d{2}:\d{2}\.\d{3})\s+-->\s+" +
        @"(?<end>(?:\d{2,}:)?\d{2}:\d{2}\.\d{3})(?:\s|$)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant,
        TimeSpan.FromMilliseconds(100));

    private static readonly Regex Markup = new(
        @"<[^>\r\n]*>", RegexOptions.Compiled | RegexOptions.CultureInvariant,
        TimeSpan.FromMilliseconds(100));

    public Task<IReadOnlyList<TextSubtitleDescriptor>> DiscoverAsync(
        string mediaPath, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        return Task.Run<IReadOnlyList<TextSubtitleDescriptor>>(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var directory = Path.GetDirectoryName(mediaPath);
            if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory))
                return Array.Empty<TextSubtitleDescriptor>();

            var stem = Path.GetFileNameWithoutExtension(mediaPath);
            var results = new List<TextSubtitleDescriptor>(32);
            try
            {
                foreach (var path in Directory.EnumerateFiles(
                    directory, stem + "*.vtt", SearchOption.TopDirectoryOnly))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    var name = Path.GetFileName(path);
                    if (!TryLanguage(stem, name, out var tag)) continue;
                    results.Add(new TextSubtitleDescriptor(
                        Id, path, name, tag, TextSubtitleSourceKind.LocalSidecar));
                    if (results.Count >= 32) break;
                }
            }
            catch (IOException) { return Array.Empty<TextSubtitleDescriptor>(); }
            catch (UnauthorizedAccessException) { return Array.Empty<TextSubtitleDescriptor>(); }
            return results.OrderBy(x => x.DisplayName, StringComparer.OrdinalIgnoreCase)
                .ToArray();
        }, cancellationToken);
    }

    public async Task<ITextSubtitleTrack> LoadAsync(
        string mediaPath, TextSubtitleDescriptor descriptor, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        ArgumentNullException.ThrowIfNull(descriptor);
        cancellationToken.ThrowIfCancellationRequested();
        if (descriptor.SourceId != Id || descriptor.Kind != TextSubtitleSourceKind.LocalSidecar)
            throw new ArgumentException("Track does not belong to WebVTT.", nameof(descriptor));

        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        var media = Path.GetFullPath(mediaPath);
        var subtitle = Path.GetFullPath(descriptor.TrackId);
        if (!string.Equals(Path.GetDirectoryName(media), Path.GetDirectoryName(subtitle),
                comparison) ||
            !TryLanguage(Path.GetFileNameWithoutExtension(media),
                Path.GetFileName(subtitle), out _))
            throw new ArgumentException("WebVTT must be a sidecar for this media.", nameof(descriptor));

        return await ReadAsync(subtitle, cancellationToken);
    }

    public async Task<ITextSubtitleTrack> LoadPreferredAsync(
        string mediaPath, CultureInfo culture, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        ArgumentNullException.ThrowIfNull(culture);
        for (var current = culture; !string.IsNullOrEmpty(current.Name);
             current = current.Parent)
        {
            var track = await ReadAsync(Path.ChangeExtension(mediaPath,
                "." + current.Name + ".vtt"), cancellationToken);
            if (track is SidecarSubtitleTrack parsed && parsed.Count != 0)
                return parsed;
        }

        return await ReadAsync(Path.ChangeExtension(mediaPath, ".vtt"), cancellationToken);
    }

    private static bool TryLanguage(string stem, string name, out string? tag)
    {
        tag = null;
        if (name.Equals(stem + ".vtt", StringComparison.OrdinalIgnoreCase))
            return true;
        var prefix = stem + ".";
        if (!name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ||
            !name.EndsWith(".vtt", StringComparison.OrdinalIgnoreCase))
            return false;

        var token = name.Substring(prefix.Length, name.Length - prefix.Length - 4);
        if (token.Length is < 2 or > 35 ||
            !token.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'))
            return false;
        tag = token;
        return true;
    }

    private static async Task<ITextSubtitleTrack> ReadAsync(
        string path, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            if (!File.Exists(path) || new FileInfo(path).Length > 2_000_000)
                return SidecarSubtitleTrack.Empty;
            var raw = await File.ReadAllTextAsync(path, cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            return Parse(raw);
        }
        catch (IOException) { return SidecarSubtitleTrack.Empty; }
        catch (UnauthorizedAccessException) { return SidecarSubtitleTrack.Empty; }
    }

    public static SidecarSubtitleTrack Parse(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw) || raw.Length > 2_000_000)
            return SidecarSubtitleTrack.Empty;

        var lines = raw.TrimStart('\uFEFF').Replace("\r\n", "\n")
            .Replace('\r', '\n').Split('\n');
        if (lines.Length == 0 ||
            !(lines[0] == "WEBVTT" || lines[0].StartsWith("WEBVTT ", StringComparison.Ordinal)))
            return SidecarSubtitleTrack.Empty;

        // The header ends at its first blank line, before any cues or NOTE.
        var index = 1;
        while (index < lines.Length && !string.IsNullOrWhiteSpace(lines[index]))
            index++;
        var cues = new List<SidecarSubtitleTrack.Cue>();

        while (index < lines.Length && cues.Count < 5000)
        {
            while (index < lines.Length && string.IsNullOrWhiteSpace(lines[index]))
                index++;
            if (index >= lines.Length) break;

            var line = lines[index].Trim();
            if (line is "NOTE" or "STYLE" or "REGION" ||
                line.StartsWith("NOTE ", StringComparison.Ordinal))
            {
                // Header blocks and comments are not timed text.
                while (index < lines.Length && !string.IsNullOrWhiteSpace(lines[index]))
                    index++;
                continue;
            }

            var match = CueTiming.Match(line);
            if (!match.Success && index + 1 < lines.Length)
            {
                // Optional cue identifier on its own line.
                index++;
                match = CueTiming.Match(lines[index].Trim());
            }
            if (!match.Success ||
                !TryTimestamp(match.Groups["start"].Value, out var start) ||
                !TryTimestamp(match.Groups["end"].Value, out var end) ||
                end <= start)
            {
                while (index < lines.Length && !string.IsNullOrWhiteSpace(lines[index]))
                    index++;
                continue;
            }

            index++;
            var body = new List<string>(5);
            while (index < lines.Length && !string.IsNullOrWhiteSpace(lines[index]))
            {
                if (body.Count < 5)
                {
                    // Avalonia uses a TextBlock; discard cue tags and decode
                    // entities rather than exposing raw <v>/<i>/<c> markup.
                    var text = WebUtility.HtmlDecode(Markup.Replace(lines[index], "")).Trim();
                    body.Add(text);
                }
                index++;
            }
            var joined = string.Join("\n", body).Trim();
            if (joined.Length is > 0 and <= 500)
                cues.Add(new SidecarSubtitleTrack.Cue(start, end, joined));
        }
        return SidecarSubtitleTrack.FromCues(cues);
    }

    private static bool TryTimestamp(string raw, out TimeSpan time)
    {
        time = default;
        var parts = raw.Split(':');
        if (parts.Length is not (2 or 3)) return false;
        var hours = 0L;
        var offset = 0;
        if (parts.Length == 3)
        {
            if (!long.TryParse(parts[0], NumberStyles.None,
                    CultureInfo.InvariantCulture, out hours)) return false;
            offset = 1;
        }
        if (!int.TryParse(parts[offset], NumberStyles.None,
                CultureInfo.InvariantCulture, out var minutes) ||
            minutes is < 0 or > 59) return false;
        var secondParts = parts[offset + 1].Split('.');
        if (secondParts.Length != 2 ||
            !int.TryParse(secondParts[0], NumberStyles.None,
                CultureInfo.InvariantCulture, out var seconds) ||
            seconds is < 0 or > 59 ||
            secondParts[1].Length != 3 ||
            !int.TryParse(secondParts[1], NumberStyles.None,
                CultureInfo.InvariantCulture, out var milliseconds)) return false;

        try
        {
            var ticks = checked(hours * TimeSpan.TicksPerHour +
                                minutes * TimeSpan.TicksPerMinute +
                                seconds * TimeSpan.TicksPerSecond +
                                milliseconds * TimeSpan.TicksPerMillisecond);
            time = new TimeSpan(ticks);
            return true;
        }
        catch (OverflowException) { return false; }
    }
}
