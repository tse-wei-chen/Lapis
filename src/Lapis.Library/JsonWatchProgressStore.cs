using System.Text.Json;
using System.Text.Json.Serialization;

namespace Lapis.Library;

/// <summary>
/// Playback bookmarks are deliberately independent of the media catalog and
/// native execution lifecycle. Only seekable local-file titles are recorded.
/// </summary>
public sealed record WatchProgress(
    Guid ItemId, double PositionSeconds, double DurationSeconds, DateTimeOffset UpdatedAtUtc)
{
    [JsonIgnore]
    public bool IsResumable => ItemId != Guid.Empty &&
        double.IsFinite(PositionSeconds) && double.IsFinite(DurationSeconds) &&
        DurationSeconds >= 60 && PositionSeconds >= 20 &&
        DurationSeconds - PositionSeconds >= 30 &&
        PositionSeconds <= DurationSeconds * 0.95;
}

/// <summary>
/// A single-process, atomic sidecar journal. Failed/unsupported records never
/// become playable bookmarks. A completed title removes its old bookmark.
/// </summary>
public sealed class JsonWatchProgressStore(string path)
{
    private const int Version = 1;
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
        try { return (await ReadAsync(cancellationToken)).ToArray(); }
        finally { _gate.Release(); }
    }

    public async Task SaveAsync(WatchProgress progress,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(progress);
        if (progress.ItemId == Guid.Empty) return;
        await _gate.WaitAsync(cancellationToken);
        try
        {
            var items = await ReadAsync(cancellationToken);
            items.RemoveAll(item => item.ItemId == progress.ItemId);
            if (progress.IsResumable)
                items.Add(progress);
            await WriteAsync(items, cancellationToken);
        }
        finally { _gate.Release(); }
    }

    private async Task<List<WatchProgress>> ReadAsync(CancellationToken ct)
    {
        if (!File.Exists(_path)) return [];
        try
        {
            await using var stream = new FileStream(_path, FileMode.Open, FileAccess.Read,
                FileShare.Read, 4096, useAsync: true);
            var payload = await JsonSerializer.DeserializeAsync<Envelope>(stream, Options, ct);
            if (payload is null || payload.Version != Version || payload.Items is null)
                throw new InvalidDataException("觀影紀錄版本不相容；原始資料不會被覆寫。");
            return payload.Items
                .Where(item => item is not null && item.IsResumable)
                .GroupBy(item => item.ItemId)
                .Select(group => group.MaxBy(item => item.UpdatedAtUtc)!)
                .ToList();
        }
        catch (JsonException ex)
        {
            throw new InvalidDataException("觀影紀錄格式損壞；原始資料不會被覆寫。", ex);
        }
    }

    private async Task WriteAsync(List<WatchProgress> items, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var temporary = _path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await using (var stream = new FileStream(temporary, FileMode.CreateNew,
                FileAccess.Write, FileShare.None, 4096, useAsync: true))
            {
                await JsonSerializer.SerializeAsync(stream, new Envelope(Version, items), Options, ct);
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

    private sealed record Envelope(int Version, List<WatchProgress> Items);
}
