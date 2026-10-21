using System.Threading;

namespace Lapis.Contracts.Media;

/// <summary>
/// A generated cue in media time, not wall-clock time. The producer (ASR,
/// translation or IPC) must convert timestamps before publishing.
/// </summary>
public sealed record GeneratedSubtitleCue(TimeSpan Start, TimeSpan End, string Text);

/// <summary>
/// A bounded, nonblocking, single-media-session subtitle snapshot.
/// Producers replace immutable snapshots; the render/UI reader never awaits
/// recognition, translation or a native decoder. Epoch/generation fencing
/// prevents late ASR results from appearing after Seek or media replacement.
/// </summary>
public sealed class GeneratedSubtitleTimeline : ITextSubtitleTrack
{
    public const int MaxCues = 512;
    public const int MaxTextLength = 4096;
    private readonly object _writeGate = new();
    private long _generation;
    private Snapshot _snapshot = new(0, -1, [], []);
    private sealed record Snapshot(
        long Generation, long Revision, GeneratedSubtitleCue[] Cues,
        long[] PrefixMaxEnd);

    /// <summary>Invalidate all prior work; call on Seek, Open and Close.</summary>
    public long BeginGeneration()
    {
        lock (_writeGate)
        {
            if (_generation == long.MaxValue)
                throw new InvalidOperationException("Subtitle generation exhausted.");
            var next = ++_generation;
            Volatile.Write(ref _snapshot, new Snapshot(next, -1, [], []));
            return next;
        }
    }

    /// <summary>
    /// Atomically publish an owned copy of a complete rolling cue window.
    /// Returns false for an obsolete generation or non-increasing revision.
    /// Malformed batches are rejected without changing the previous snapshot.
    /// </summary>
    public bool TryPublish(
        long generation, long revision, IReadOnlyList<GeneratedSubtitleCue> cues)
    {
        ArgumentNullException.ThrowIfNull(cues);
        if (revision < 0) throw new ArgumentOutOfRangeException(nameof(revision));
        if (cues.Count > MaxCues)
            throw new ArgumentException("Generated subtitle window exceeds cue limit.", nameof(cues));

        var copy = new GeneratedSubtitleCue[cues.Count];
        for (var i = 0; i < copy.Length; i++)
        {
            var cue = cues[i] ?? throw new ArgumentException("Null subtitle cue.", nameof(cues));
            if (cue.Start < TimeSpan.Zero || cue.End <= cue.Start ||
                string.IsNullOrWhiteSpace(cue.Text) || cue.Text.Length > MaxTextLength)
                throw new ArgumentException("Invalid generated subtitle cue.", nameof(cues));
            copy[i] = cue;
        }
        Array.Sort(copy, static (a, b) =>
        {
            var compare = a.Start.CompareTo(b.Start);
            return compare != 0 ? compare : a.End.CompareTo(b.End);
        });
        var prefix = new long[copy.Length];
        long maximum = long.MinValue;
        for (var i = 0; i < copy.Length; i++)
        {
            maximum = Math.Max(maximum, copy[i].End.Ticks);
            prefix[i] = maximum;
        }

        lock (_writeGate)
        {
            var current = Volatile.Read(ref _snapshot);
            if (generation <= 0 || generation != _generation || revision <= current.Revision)
                return false;
            Volatile.Write(ref _snapshot, new Snapshot(generation, revision, copy, prefix));
            return true;
        }
    }

    public string? At(TimeSpan position)
    {
        if (position < TimeSpan.Zero) return null;
        var snapshot = Volatile.Read(ref _snapshot);
        var cues = snapshot.Cues;
        // Find last cue whose start <= playback time. Overlapping cues are
        // resolved by latest start; prefix maxima allow early termination.
        var lo = 0;
        var hi = cues.Length;
        while (lo < hi)
        {
            var mid = lo + (hi - lo) / 2;
            if (cues[mid].Start <= position) lo = mid + 1;
            else hi = mid;
        }
        for (var i = lo - 1; i >= 0; i--)
        {
            if (snapshot.PrefixMaxEnd[i] <= position.Ticks) break;
            if (cues[i].End > position) return cues[i].Text;
        }
        return null;
    }
}
