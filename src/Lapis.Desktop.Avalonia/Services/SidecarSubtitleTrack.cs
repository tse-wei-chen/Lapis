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
    // Segment-tree maximum end time: overlapping SRT cues are not ordered by End.
    // This lets us find the latest-starting active cue without scanning all
    // earlier subtitles on every render tick or after a backward seek.
    private readonly TimeSpan[] _maximumEnds;
    private readonly int _leafCount;

    private SidecarSubtitleTrack(Cue[] cues)
    {
        _cues = cues;
        var leaves = 1;
        while (leaves < cues.Length) leaves <<= 1;
        _leafCount = leaves;
        _maximumEnds = new TimeSpan[leaves * 2];

        for (var i = 0; i < cues.Length; i++)
            _maximumEnds[leaves + i] = cues[i].End;
        for (var i = leaves - 1; i > 0; i--)
        {
            var left = _maximumEnds[i * 2];
            var right = _maximumEnds[i * 2 + 1];
            _maximumEnds[i] = left >= right ? left : right;
        }
    }

    public static SidecarSubtitleTrack Empty { get; } = new([]);
    public int Count => _cues.Length;

    public string? At(TimeSpan time)
    {
        // Find the exclusive upper bound of all cues that have started.
        // Searching by End is incorrect: nested or overlapping cues can have
        // non-monotonic end times even after sorting by Start.
        var low = 0;
        var high = _cues.Length;
        while (low < high)
        {
            var mid = low + ((high - low) / 2);
            if (_cues[mid].Start <= time) low = mid + 1;
            else high = mid;
        }

        // When cues overlap, prefer the most recently started active cue.
        // End is exclusive, matching normal SRT boundary behavior.
        var index = FindLatestActive(1, 0, _leafCount, low, time);
        return index < 0 ? null : _cues[index].Text;
    }

    private int FindLatestActive(int node, int begin, int end, int startedBefore, TimeSpan time)
    {
        if (begin >= startedBefore || _maximumEnds[node] <= time)
            return -1;
        if (end - begin == 1) return begin;

        var middle = begin + ((end - begin) / 2);
        var right = FindLatestActive(node * 2 + 1, middle, end, startedBefore, time);
        return right >= 0
            ? right
            : FindLatestActive(node * 2, begin, middle, startedBefore, time);
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
