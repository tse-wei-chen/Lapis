namespace Lapis.Library;

/// <summary>
/// Lists file names of MPLS descriptors only. It does not parse clip order,
/// distinguish bonus titles or implement playlist selection / BD-J menus.
/// </summary>
public static class DiscPlaylistIndex
{
    public static IReadOnlyList<string> ListIdentifiers(MediaLibraryItem item)
    {
        if (!item.IsDisc || !item.IsAvailable) return [];
        var folder = Path.Combine(item.SourcePath, "BDMV", "PLAYLIST");
        if (!Directory.Exists(folder)) return [];
        try
        {
            return Directory.EnumerateFiles(folder, "*.mpls", SearchOption.TopDirectoryOnly)
                .Select(Path.GetFileNameWithoutExtension)
                .Where(name => name is not null)
                .Select(name => name!)
                .OrderBy(name => name, StringComparer.OrdinalIgnoreCase)
                .Take(200)
                .ToArray();
        }
        catch (IOException) { return []; }
        catch (UnauthorizedAccessException) { return []; }
    }
}
