namespace Lapis.Library;

/// <summary>
/// Disc-supplied art is a local optional preview, not a trusted movie-poster
/// service. Probe only BDMV/META/DL (never BACKUP or MAKEMKV), without parsing
/// arbitrary XML or loading any image bytes on the UI thread.
/// </summary>
public static class DiscArtworkLocator
{
    public const long MaxSourceBytes = 16L * 1024 * 1024;

    public static string? FindPreferredArtwork(MediaLibraryItem item)
    {
        if (!item.IsDisc || !item.IsAvailable) return null;
        var folder = Path.Combine(item.SourcePath, "BDMV", "META", "DL");
        try
        {
            if (!Directory.Exists(folder)) return null;
            return Directory.EnumerateFiles(folder, "*", SearchOption.TopDirectoryOnly)
                .Where(path => Path.GetExtension(path).Equals(".jpg",
                                   StringComparison.OrdinalIgnoreCase) ||
                               Path.GetExtension(path).Equals(".jpeg",
                                   StringComparison.OrdinalIgnoreCase))
                .Where(IsSuitable)
                .OrderBy(path => HasLargeSuffix(path) ? 0 : 1)
                .ThenBy(path => Path.GetFileName(path), StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    private static bool IsSuitable(string path)
    {
        try
        {
            var file = new FileInfo(path);
            // Reject file links and potential directory escape; no need to
            // follow links when only bundled metadata artwork is desired.
            return (file.Attributes & FileAttributes.ReparsePoint) == 0 &&
                file.Length > 0 && file.Length <= MaxSourceBytes;
        }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }

    private static bool HasLargeSuffix(string path) =>
        Path.GetFileNameWithoutExtension(path)
            .EndsWith("_L", StringComparison.OrdinalIgnoreCase);
}
