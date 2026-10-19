using Lapis.Contracts.Media;

namespace Lapis.Desktop.WinForms;

/// <summary>
/// UI-only validation for directory selection. Native Source Provider still
/// performs the authoritative BDMV check before any stream is opened.
/// </summary>
internal static class BdmvDirectorySelection
{
    public static bool TryCreate(string? selectedPath, out MediaLocator? locator)
    {
        locator = null;
        if (string.IsNullOrWhiteSpace(selectedPath))
        {
            return false;
        }

        try
        {
            var path = Path.GetFullPath(selectedPath);
            if (!Directory.Exists(path))
            {
                return false;
            }

            // Both the disc root and the BDMV directory itself are accepted.
            var isBdmv = string.Equals(
                Path.GetFileName(Path.TrimEndingDirectorySeparator(path)),
                "BDMV", StringComparison.OrdinalIgnoreCase);
            var bdmvPath = isBdmv ? path : Path.Combine(path, "BDMV");
            if (!Directory.Exists(Path.Combine(bdmvPath, "PLAYLIST")) ||
                !Directory.Exists(Path.Combine(bdmvPath, "STREAM")) ||
                (!File.Exists(Path.Combine(bdmvPath, "index.bdmv")) &&
                 !File.Exists(Path.Combine(bdmvPath, "index.bdm"))))
            {
                return false;
            }

            locator = new MediaLocator(MediaLocatorKind.Directory, path);
            return true;
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (NotSupportedException)
        {
            return false;
        }
    }
}
