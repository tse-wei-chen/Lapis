using Lapis.Contracts.Media;
using Lapis.Library;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Disc root and its BDMV child both normalize to the same catalog locator.
/// No parser/DRM inference is made from the auxiliary MakeMKV directory.
/// </summary>
public static class BdmvLocator
{
    public static bool TryFromDirectory(string? path, out MediaLocator? locator)
    {
        locator = null;
        if (string.IsNullOrWhiteSpace(path)) return false;
        try
        {
            locator = LibrarySourcePaths.Normalize(
                new MediaLocator(MediaLocatorKind.Directory, path));
            return true;
        }
        catch (ArgumentException) { return false; }
        catch (NotSupportedException) { return false; }
        catch (InvalidDataException) { return false; }
        catch (DirectoryNotFoundException) { return false; }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }
}
