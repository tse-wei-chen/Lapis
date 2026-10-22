using System.Text.Json;
using Lapis.Contracts.Media;

namespace Lapis.Library;

/// <summary>
/// Local single-process v1 catalog. Durable atomic same-volume replace keeps
/// a failed write from silently truncating the existing library.
/// IMediaLibrary permits a future SQLite adapter without UI or player changes.
/// </summary>
public sealed class JsonMediaLibrary(string catalogFilePath) : IRelinkableMediaLibrary, IEditableMediaLibrary
{
    private const int SchemaVersion = 1;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };
    private static readonly StringComparer PathComparer =
        OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
    private readonly string _path = Path.GetFullPath(catalogFilePath);
    private readonly SemaphoreSlim _gate = new(1, 1);

    public async Task<IReadOnlyList<MediaLibraryItem>> ListAsync(
        CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken);
        try { return (await ReadAsync(cancellationToken)).Items.ToArray(); }
        finally { _gate.Release(); }
    }

    public async Task<MediaLibraryItem> ImportAsync(
        MediaLocator locator, CancellationToken cancellationToken = default)
    {
        var source = LibrarySourcePaths.Normalize(locator);
        await _gate.WaitAsync(cancellationToken);
        try
        {
            var catalog = await ReadAsync(cancellationToken);
            var existing = catalog.Items.FirstOrDefault(item =>
                item.Kind == source.Kind && PathComparer.Equals(item.SourcePath, source.Value));
            if (existing is not null) return existing;

            var title = Path.GetFileName(source.Value);
            if (source.Kind == MediaLocatorKind.File)
                title = Path.GetFileNameWithoutExtension(source.Value);
            if (string.IsNullOrWhiteSpace(title)) title = source.Value;
            var imported = new MediaLibraryItem(
                Guid.NewGuid(), source.Kind, source.Value, title, DateTimeOffset.UtcNow);
            catalog.Items.Add(imported);
            await SaveAsync(catalog, cancellationToken);
            return imported;
        }
        finally { _gate.Release(); }
    }

    /// <summary>
    /// Repair a moved video or BDMV root while keeping its stable catalog ID,
    /// title, import timestamp, favorite and watch-history associations.
    /// Deliberately refuses overwriting a currently reachable source, and
    /// refuses pointing two different items at the same canonical location.
    /// </summary>
    public async Task<MediaLibraryItem> RelinkAsync(Guid itemId, MediaLocator replacement,
        CancellationToken cancellationToken = default)
    {
        if (itemId == Guid.Empty) throw new ArgumentException("無效的收藏項目。", nameof(itemId));
        var source = LibrarySourcePaths.Normalize(replacement);
        await _gate.WaitAsync(cancellationToken);
        try
        {
            var catalog = await ReadAsync(cancellationToken);
            var index = catalog.Items.FindIndex(item => item.Id == itemId);
            if (index < 0)
                throw new KeyNotFoundException("找不到要修復的收藏項目。");

            var previous = catalog.Items[index];
            if (previous.Kind != source.Kind)
                throw new InvalidOperationException("修復來源必須保持原本的媒體類型。");
            if (PathComparer.Equals(previous.SourcePath, source.Value))
                return previous; // The original drive may simply be mounted again.
            if (previous.IsAvailable)
                throw new InvalidOperationException("原始來源仍可存取，不可覆寫現有收藏路徑。");
            if (catalog.Items.Any(item => item.Id != itemId &&
                item.Kind == source.Kind && PathComparer.Equals(item.SourcePath, source.Value)))
                throw new InvalidOperationException("所選路徑已屬於另一部收藏，請勿合併不同影片。");

            var repaired = previous with { SourcePath = source.Value };
            catalog.Items[index] = repaired;
            await SaveAsync(catalog, cancellationToken);
            return repaired;
        }
        finally { _gate.Release(); }
    }

    /// <summary>
    /// Change only the user-facing title. Stable ID, path, media type and
    /// import timestamp remain unchanged, so favorites and history survive.
    /// Serialize under the same catalog gate as import/relink/remove.
    /// </summary>
    public async Task<MediaLibraryItem> RenameAsync(Guid itemId, string title,
        CancellationToken cancellationToken = default)
    {
        if (itemId == Guid.Empty)
            throw new ArgumentException("無效的收藏項目。", nameof(itemId));
        ArgumentNullException.ThrowIfNull(title);
        var trimmed = title.Trim();
        if (trimmed.Length == 0 || trimmed.Length > 160 ||
            trimmed.Any(char.IsControl))
            throw new ArgumentException(
                "電影名稱必須為 1–160 個字元，且不得包含換行或控制字元。", nameof(title));

        await _gate.WaitAsync(cancellationToken);
        try
        {
            var catalog = await ReadAsync(cancellationToken);
            var index = catalog.Items.FindIndex(item => item.Id == itemId);
            if (index < 0)
                throw new KeyNotFoundException("找不到要修改的收藏項目。");
            var existing = catalog.Items[index];
            if (existing.Title == trimmed) return existing;
            var renamed = existing with { Title = trimmed };
            catalog.Items[index] = renamed;
            await SaveAsync(catalog, cancellationToken);
            return renamed;
        }
        finally { _gate.Release(); }
    }

    public async Task<bool> RemoveAsync(Guid id, CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken);
        try
        {
            var catalog = await ReadAsync(cancellationToken);
            if (catalog.Items.RemoveAll(item => item.Id == id) == 0) return false;
            await SaveAsync(catalog, cancellationToken);
            return true;
        }
        finally { _gate.Release(); }
    }

    private async Task<Catalog> ReadAsync(CancellationToken ct)
    {
        if (!File.Exists(_path)) return new Catalog(SchemaVersion, []);
        try
        {
            await using var file = new FileStream(_path, FileMode.Open, FileAccess.Read, FileShare.Read,
                bufferSize: 4096, useAsync: true);
            var data = await JsonSerializer.DeserializeAsync<Catalog>(file, JsonOptions, ct);
            if (data is null || data.Version != SchemaVersion || data.Items is null)
                throw new InvalidDataException("收藏庫資料版本不相容，沒有覆寫原始資料。");
            return data;
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("收藏庫資料損壞，沒有覆寫原始資料。", exception);
        }
    }

    private async Task SaveAsync(Catalog catalog, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var temp = _path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await using (var file = new FileStream(temp, FileMode.CreateNew,
                FileAccess.Write, FileShare.None, bufferSize: 4096, useAsync: true))
            {
                await JsonSerializer.SerializeAsync(file, catalog, JsonOptions, ct);
                await file.FlushAsync(ct);
                file.Flush(flushToDisk: true);
            }
            ct.ThrowIfCancellationRequested();
            File.Move(temp, _path, overwrite: true);
        }
        finally
        {
            if (File.Exists(temp)) File.Delete(temp);
        }
    }

    private sealed record Catalog(int Version, List<MediaLibraryItem> Items);
}
