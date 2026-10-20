using System.Globalization;
using System.Text.RegularExpressions;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Optional local, unprotected sidecar SRT cues. This does not claim embedded
/// PGS/BDMV/ASS support. SRT text is displayed over the video without a box.
/// </summary>
public sealed class SidecarSubtitleTrack
{
    public sealed record Cue(TimeSpan Start, TimeSpan End, string Text);
    private static readonly Regex Timing = new(
        @"^(?<s>\d{2}:\d{2}:\d{2}[,.]\d{3})\s+-->\s+(?<e>\d{2}:\d{2}:\d{2}[,.]\d{3})",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private readonly Cue[] _cues;

    private SidecarSubtitleTrack(Cue[] cues) => _cues = cues;
    public static SidecarSubtitleTrack Empty { get; } = new([]);
    public int Count => _cues.Length;

    public string? At(TimeSpan time)
    {
        // Cue count is bounded; a binary search is more stable while seeking.
        var low = 0;
        var high = _cues.Length - 1;
        while (low <= high)
        {
            var mid = low + ((high - low) / 2);
            var cue = _cues[mid];
            if (time < cue.Start) high = mid - 1;
            else if (time >= cue.End) low = mid + 1;
            else return cue.Text;
        }
        return null;
    }

    public static SidecarSubtitleTrack Parse(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return Empty;
        if (raw.Length > 2_000_000) return Empty;
        var cues = new List<Cue>();
        var lines = raw.Replace("\r\n", "\n").Replace('\r', '\n')
            .Split('\n');
        for (var i = 0; i < lines.Length && cues.Count < 5000; i++)
        {
            var match = Timing.Match(lines[i].Trim());
            if (!match.Success ||
                !TryTime(match.Groups["s"].Value, out var start) ||
                !TryTime(match.Groups["e"].Value, out var end) ||
                end <= start) continue;
            var body = new List<string>();
            while (++i < lines.Length && !string.IsNullOrWhiteSpace(lines[i]))
            {
                if (body.Count < 5) body.Add(lines[i].Trim());
            }
            var text = string.Join("\n", body).Trim();
            if (text.Length is > 0 and <= 500)
                cues.Add(new Cue(start, end, text));
        }
        return new SidecarSubtitleTrack(cues.OrderBy(c => c.Start).ToArray());
    }

    private static bool TryTime(string text, out TimeSpan value) =>
        TimeSpan.TryParseExact(text.Replace(',', '.'), @"hh\:mm\:ss\.fff",
            CultureInfo.InvariantCulture, out value);

    public static async Task<SidecarSubtitleTrack> LoadForFileAsync(
        string videoPath, CancellationToken cancellationToken = default)
    {
        var candidate = Path.ChangeExtension(videoPath, ".srt");
        try
        {
            if (!File.Exists(candidate) || new FileInfo(candidate).Length > 2_000_000)
                return Empty;
            var text = await File.ReadAllTextAsync(candidate, cancellationToken);
            return Parse(text);
        }
        catch (IOException) { return Empty; }
        catch (UnauthorizedAccessException) { return Empty; }
    }
}
