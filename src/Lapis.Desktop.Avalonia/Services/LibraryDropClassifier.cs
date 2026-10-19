using Lapis.Contracts.Media;
using Lapis.Library;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Classify Explorer drop paths without opening a native playback session.
/// Disc-root and BDMV-child paths canonicalize through the one catalog rule;
/// an arbitrary directory is not assumed to be a video collection.
/// </summary>
public static class LibraryDropClassifier
{
    public const int MaxDroppedItems = 200;

    private static readonly HashSet<string> VideoExtensions = new(
    [
        ".mkv", ".mp4", ".m4v", ".mov", ".avi", ".wmv", ".webm",
        ".mpg", ".mpeg", ".ts", ".m2ts", ".mts", ".flv", ".vob",
        ".ogv", ".3gp", ".3g2", ".mxf"
    ], StringComparer.OrdinalIgnoreCase);

    private static readonly StringComparer PathComparer =
        OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    public static LibraryDropPlan Classify(IEnumerable<string?> paths)
    {
        ArgumentNullException.ThrowIfNull(paths);
        var sources = new List<MediaLocator>();
        var rejected = new List<string>();
        var unique = new HashSet<string>(PathComparer);
        var count = 0;
        foreach (var input in paths)
        {
            if (++count > MaxDroppedItems)
            {
                rejected.Add($"超過單次 {MaxDroppedItems} 個項目的上限；請分批拖入。");
                break;
            }

            if (string.IsNullOrWhiteSpace(input))
            {
                rejected.Add("無法辨識非本機來源。");
                continue;
            }

            try
            {
                MediaLocator candidate;
                if (Directory.Exists(input))
                {
                    // Directory validation is shared by picker, library and
                    // drag-drop, including BDMV sibling CERTIFICATE/MAKEMKV.
                    candidate = LibrarySourcePaths.Normalize(
                        new MediaLocator(MediaLocatorKind.Directory, input));
                }
                else if (File.Exists(input))
                {
                    if (!VideoExtensions.Contains(Path.GetExtension(input)))
                    {
                        rejected.Add($"不支援的影片格式：{Path.GetFileName(input)}");
                        continue;
                    }
                    candidate = LibrarySourcePaths.Normalize(
                        new MediaLocator(MediaLocatorKind.File, input));
                }
                else
                {
                    rejected.Add($"來源不存在或無法存取：{Path.GetFileName(input)}");
                    continue;
                }

                var key = $"{(int)candidate.Kind}:{candidate.Value}";
                if (unique.Add(key)) sources.Add(candidate);
            }
            catch (Exception ex) when (ex is ArgumentException or IOException or
                InvalidDataException or UnauthorizedAccessException or NotSupportedException)
            {
                rejected.Add($"無法匯入 {Path.GetFileName(input)}：{ex.Message}");
            }
        }

        return new LibraryDropPlan(sources, rejected);
    }
}

public sealed record LibraryDropPlan(
    IReadOnlyList<MediaLocator> Sources,
    IReadOnlyList<string> Rejected);
