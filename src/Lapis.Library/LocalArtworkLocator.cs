using Lapis.Contracts.Media;

namespace Lapis.Library;

/// <summary>
/// Local, opt-in artwork discovery. Never calls a network metadata service,
/// scans outside the movie's immediate folder, or decodes image bytes.
/// Broken candidates can be skipped by the bounded desktop image decoder.
/// </summary>
public static class LocalArtworkLocator
{
    public const long MaxSourceBytes = DiscArtworkLocator.MaxSourceBytes;

    private static readonly string[] Extensions = [".jpg", ".jpeg", ".png"];
    private static readonly HashSet<string> VideoExtensions = new(
        [".mp4", ".mkv", ".mov", ".avi", ".wmv", ".webm", ".m2ts", ".ts"],
        StringComparer.OrdinalIgnoreCase);

    public static IReadOnlyList<string> FindCandidates(MediaLibraryItem item)
    {
        ArgumentNullException.ThrowIfNull(item);
        if (!item.IsAvailable) return [];
        if (item.IsDisc)
        {
            var disc = DiscArtworkLocator.FindPreferredArtwork(item);
            return disc is null ? [] : [disc];
        }
        if (item.Kind != MediaLocatorKind.File) return [];

        var directory = Path.GetDirectoryName(item.SourcePath);
        if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory)) return [];

        try
        {
            var basename = Path.GetFileNameWithoutExtension(item.SourcePath);
            var priority = new[]
            {
                basename + ".poster", basename + ".cover", basename,
                "poster", "folder", "cover"
            };
            var files = Directory.EnumerateFiles(directory, "*", SearchOption.TopDirectoryOnly)
                .ToArray();

            // A shared "folder.jpg" should not silently appear as the poster
            // of every different film in a flat video directory.
            var isSingleMovieDirectory = files.Count(file =>
                VideoExtensions.Contains(Path.GetExtension(file))) <= 1;

            return files.Where(path => Extensions.Contains(Path.GetExtension(path),
                        StringComparer.OrdinalIgnoreCase) && IsSuitable(path))
                .Select(path => new
                {
                    Path = path,
                    Rank = Array.FindIndex(priority,
                        title => title.Equals(Path.GetFileNameWithoutExtension(path),
                            StringComparison.OrdinalIgnoreCase))
                })
                .Where(entry => entry.Rank >= 0 &&
                    (entry.Rank < 3 || isSingleMovieDirectory))
                .OrderBy(entry => entry.Rank)
                .ThenBy(entry => Array.FindIndex(Extensions,
                    extension => extension.Equals(Path.GetExtension(entry.Path),
                        StringComparison.OrdinalIgnoreCase)))
                .ThenBy(entry => entry.Path, StringComparer.OrdinalIgnoreCase)
                .Select(entry => entry.Path)
                .ToArray();
        }
        catch (IOException) { return []; }
        catch (UnauthorizedAccessException) { return []; }
    }

    private static bool IsSuitable(string path)
    {
        try
        {
            var file = new FileInfo(path);
            return (file.Attributes & FileAttributes.ReparsePoint) == 0 &&
                file.Length > 0 && file.Length <= MaxSourceBytes;
        }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }
}
