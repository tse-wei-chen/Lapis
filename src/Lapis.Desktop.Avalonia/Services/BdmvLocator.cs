using Lapis.Contracts.Media;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Cheap local UX validation only; the Native Source Provider remains the
/// authoritative BDMV parser and enforces clear-disc/protection constraints.
/// </summary>
public static class BdmvLocator
{
    public static bool TryFromDirectory(string? path, out MediaLocator? locator)
    {
        locator = null;
        if (string.IsNullOrWhiteSpace(path)) return false;
        try
        {
            var absolute = Path.GetFullPath(path);
            if (!Directory.Exists(absolute)) return false;
            var last = Path.GetFileName(Path.TrimEndingDirectorySeparator(absolute));
            var bdmv = string.Equals(last, "BDMV", StringComparison.OrdinalIgnoreCase)
                ? absolute : Path.Combine(absolute, "BDMV");
            if (!Directory.Exists(Path.Combine(bdmv, "PLAYLIST")) ||
                !Directory.Exists(Path.Combine(bdmv, "STREAM")) ||
                (!File.Exists(Path.Combine(bdmv, "index.bdmv")) &&
                 !File.Exists(Path.Combine(bdmv, "index.bdm"))))
                return false;

            locator = new MediaLocator(MediaLocatorKind.Directory, absolute);
            return true;
        }
        catch (ArgumentException) { return false; }
        catch (NotSupportedException) { return false; }
        catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }
}
