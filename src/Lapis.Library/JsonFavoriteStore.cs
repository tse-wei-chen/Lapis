using System.Text.Json;
using System.Text.Json.Serialization;

namespace Lapis.Library;

/// <summary>A pinned catalog item; never changes the source movie or BDMV.</summary>
public sealed record FavoriteEntry(Guid ItemId, DateTimeOffset AddedAtUtc)
{
    [JsonIgnore]
    public bool IsValid => ItemId != Guid.Empty && AddedAtUtc != default;
}

/// <summary>
/// Independent, single-process, atomic sidecar for user-curated favorites.
/// Unknown schema versions and corrupt files are never silently overwritten.
/// </summary>
public sealed class JsonFavoriteStore(string path)
{
    private const int Version = 1;
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };
    private readonly string _path = Path.GetFullPath(path);
    private readonly SemaphoreSlim _gate = new(1, 1);

    public async Task<IReadOnlyList<FavoriteEntry>> LoadAsync(
        CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken);
        try { return (await ReadAsync(cancellationToken)).ToArray(); }
        finally { _gate.Release(); }
    }

    public async Task SetAsync(Guid itemId, bool favorite,
        CancellationToken cancellationToken = default)
    {
        if (itemId == Guid.Empty) throw new ArgumentException("收藏項目 ID 不可為空。", nameof(itemId));
        await _gate.WaitAsync(cancellationToken);
        try
        {
            var entries = await ReadAsync(cancellationToken);
            var current = entries.FindIndex(item => item.ItemId == itemId);
            if (favorite && current >= 0 || !favorite && current < 0) return;

            if (favorite)
                entries.Add(new FavoriteEntry(itemId, DateTimeOffset.UtcNow));
            else
                entries.RemoveAt(current);
            await WriteAsync(entries, cancellationToken);
        }
        finally { _gate.Release(); }
    }

    private async Task<List<FavoriteEntry>> ReadAsync(CancellationToken ct)
    {
        if (!File.Exists(_path)) return [];
        try
        {
            await using var stream = new FileStream(_path, FileMode.Open,
                FileAccess.Read, FileShare.Read, 4096, useAsync: true);
            var envelope = await JsonSerializer.DeserializeAsync<Envelope>(stream, Options, ct);
            if (envelope is null || envelope.Version != Version || envelope.Items is null)
                throw new InvalidDataException("最愛收藏版本不相容；原始資料未遭覆寫。");

            return envelope.Items.Where(item => item is not null && item.IsValid)
                .GroupBy(item => item.ItemId)
                .Select(group => group.MaxBy(item => item.AddedAtUtc)!)
                .OrderByDescending(item => item.AddedAtUtc)
                .ThenBy(item => item.ItemId)
                .ToList();
        }
        catch (JsonException ex)
        {
            throw new InvalidDataException("最愛收藏格式損壞；原始資料未遭覆寫。", ex);
        }
    }

    private async Task WriteAsync(List<FavoriteEntry> entries, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var temporary = _path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await using (var stream = new FileStream(temporary,
                FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, useAsync: true))
            {
                await JsonSerializer.SerializeAsync(stream, new Envelope(Version, entries),
                    Options, ct);
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

    private sealed record Envelope(int Version, List<FavoriteEntry> Items);
}
