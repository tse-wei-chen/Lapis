using System.Text.Json;
using System.Text.Json.Serialization;

namespace Lapis.Library;

/// <summary>
/// A lightweight, per-catalog watch snapshot. Completion means the playback
/// reached the final 30 seconds or at least 95% of a seekable local movie.
/// </summary>
public sealed record WatchProgress(
    Guid ItemId, double PositionSeconds, double DurationSeconds, DateTimeOffset UpdatedAtUtc)
{
    [JsonIgnore]
    public bool IsHistoryEligible => ItemId != Guid.Empty &&
        UpdatedAtUtc != default && double.IsFinite(PositionSeconds) &&
        double.IsFinite(DurationSeconds) && DurationSeconds >= 60 &&
        PositionSeconds >= 20 && PositionSeconds <= DurationSeconds;

    [JsonIgnore]
    public bool IsCompleted => IsHistoryEligible &&
        (PositionSeconds >= DurationSeconds * 0.95 ||
         (PositionSeconds >= DurationSeconds * 0.85 &&
          DurationSeconds - PositionSeconds <= 30));

    [JsonIgnore]
    public bool IsResumable => IsHistoryEligible && !IsCompleted;
}

/// <summary>
/// Local single-process v2 journal. Pre-release legacy formats are not
/// supported. Unknown versions fail closed rather than being overwritten.
/// Completed movies stay in History while disappearing from resumable Items.
/// The catalog and Native actor never depend on this format.
/// </summary>
public sealed class JsonWatchProgressStore(string path)
{
    private const int Version = 2;
    private const int MaxHistoryItems = 200;
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };
    private readonly string _path = Path.GetFullPath(path);
    private readonly SemaphoreSlim _gate = new(1, 1);

    public async Task<IReadOnlyList<WatchProgress>> LoadAsync(
        CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken);
        try { return (await ReadAsync(cancellationToken)).Bookmarks.ToArray(); }
        finally { _gate.Release(); }
    }

    public async Task<IReadOnlyList<WatchProgress>> LoadHistoryAsync(
        CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken);
        try { return (await ReadAsync(cancellationToken)).History.ToArray(); }
        finally { _gate.Release(); }
    }

    /// <summary>
    /// Write an eligible observation only if it is newer than the stored
    /// observation for the same movie. A delayed timer callback must not
    /// resurrect an earlier resume position after completion or rewind
    /// a newer seek. The result tells UI callers if the snapshot was
    /// actually persisted, preventing stale in-memory shelf updates.
    /// </summary>
    public async Task<bool> SaveAsync(WatchProgress progress,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(progress);
        // A glance or failed playback must not destroy a previous bookmark.
        if (!progress.IsHistoryEligible) return false;
        await _gate.WaitAsync(cancellationToken);
        try
        {
            var state = await ReadAsync(cancellationToken);
            // Look at history, not bookmarks: a completed movie deliberately
            // has no resume bookmark, but still has a latest timestamp.
            var existing = state.History.Find(item => item.ItemId == progress.ItemId);
            if (existing is not null && existing.UpdatedAtUtc >= progress.UpdatedAtUtc)
                return false;

            state.Bookmarks.RemoveAll(item => item.ItemId == progress.ItemId);
            if (progress.IsResumable)
                state.Bookmarks.Add(progress);

            state.History.RemoveAll(item => item.ItemId == progress.ItemId);
            state.History.Add(progress);
            TrimHistory(state.History);
            await WriteAsync(state, cancellationToken);
            return true;
        }
        finally { _gate.Release(); }
    }

    /// <summary>
    /// Privacy action: forget this title completely (history and bookmark).
    /// Repeating the action is safe, and never touches the media file.
    /// </summary>
    public async Task ForgetAsync(Guid itemId, CancellationToken cancellationToken = default)
    {
        if (itemId == Guid.Empty) return;
        await _gate.WaitAsync(cancellationToken);
        try
        {
            var state = await ReadAsync(cancellationToken);
            var bookmarks = state.Bookmarks.RemoveAll(item => item.ItemId == itemId);
            var history = state.History.RemoveAll(item => item.ItemId == itemId);
            if (bookmarks != 0 || history != 0)
                await WriteAsync(state, cancellationToken);
        }
        finally { _gate.Release(); }
    }

    private async Task<State> ReadAsync(CancellationToken ct)
    {
        if (!File.Exists(_path)) return new State([], []);
        try
        {
            await using var stream = new FileStream(_path, FileMode.Open, FileAccess.Read,
                FileShare.Read, 4096, useAsync: true);
            var payload = await JsonSerializer.DeserializeAsync<Envelope>(stream, Options, ct);
            if (payload is null || payload.Version != Version ||
                payload.Items is null || payload.History is null)
                throw new InvalidDataException("觀影紀錄資料版本不相容；沒有覆寫原始資料。");

            var bookmarks = Normalize(payload.Items.Where(item => item?.IsResumable == true));
            var history = Normalize(payload.History
                .Where(item => item?.IsHistoryEligible == true));
            TrimHistory(history);
            return new State(bookmarks, history);
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("觀影紀錄格式損壞；沒有覆寫原始資料。", exception);
        }
    }

    private static List<WatchProgress> Normalize(IEnumerable<WatchProgress> items) =>
        items.GroupBy(item => item.ItemId)
            .Select(group => group.MaxBy(item => item.UpdatedAtUtc)!)
            .OrderByDescending(item => item.UpdatedAtUtc)
            .ThenBy(item => item.ItemId)
            .ToList();

    private static void TrimHistory(List<WatchProgress> history)
    {
        var compact = Normalize(history).Take(MaxHistoryItems).ToArray();
        history.Clear();
        history.AddRange(compact);
    }

    private async Task WriteAsync(State state, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var temporary = _path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await using (var stream = new FileStream(temporary, FileMode.CreateNew,
                FileAccess.Write, FileShare.None, 4096, useAsync: true))
            {
                await JsonSerializer.SerializeAsync(stream,
                    new Envelope(Version, state.Bookmarks, state.History), Options, ct);
                await stream.FlushAsync(ct);
                stream.Flush(flushToDisk: true);
            }
            ct.ThrowIfCancellationRequested();
            File.Move(temporary, _path, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    private sealed record State(List<WatchProgress> Bookmarks, List<WatchProgress> History);
    private sealed record Envelope(int Version, List<WatchProgress> Items,
        List<WatchProgress> History);
}
