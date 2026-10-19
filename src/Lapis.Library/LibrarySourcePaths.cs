using Lapis.Contracts.Media;

namespace Lapis.Library;

/// <summary>
/// Source normalization belongs to the catalog, not UI or the playback actor.
/// Both DiscRoot and DiscRoot/BDMV represent one library movie.
/// The native libbluray adapter remains the authoritative playback parser.
/// </summary>
public static class LibrarySourcePaths
{
    public static MediaLocator Normalize(MediaLocator locator)
    {
        ArgumentNullException.ThrowIfNull(locator);
        if (locator.Kind is not (MediaLocatorKind.File or MediaLocatorKind.Directory))
            throw new NotSupportedException("收藏庫目前僅支援本機影片及完整 BDMV 資料夾。");

        var path = Path.TrimEndingDirectorySeparator(Path.GetFullPath(locator.Value));
        if (locator.Kind == MediaLocatorKind.File)
        {
            if (!File.Exists(path))
                throw new FileNotFoundException("找不到原始影片檔案。", path);
            return new MediaLocator(MediaLocatorKind.File, path);
        }

        if (!Directory.Exists(path))
            throw new DirectoryNotFoundException("找不到 Blu-ray 光碟資料夾。");

        if (string.Equals(Path.GetFileName(path), "BDMV", StringComparison.OrdinalIgnoreCase))
            path = Path.GetDirectoryName(path)
                ?? throw new InvalidDataException("無效的 BDMV 光碟根目錄。");

        var bdmv = Path.Combine(path, "BDMV");
        if (!Directory.Exists(Path.Combine(bdmv, "PLAYLIST")) ||
            !Directory.Exists(Path.Combine(bdmv, "STREAM")) ||
            (!File.Exists(Path.Combine(bdmv, "index.bdmv")) &&
             !File.Exists(Path.Combine(bdmv, "index.bdm"))))
            throw new InvalidDataException("此資料夾缺少完整的 BDMV 索引、播放清單或影片串流。");

        return new MediaLocator(MediaLocatorKind.Directory, path);
    }
}
