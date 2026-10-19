using System.Text.Json.Serialization;
using Lapis.Contracts.Media;

namespace Lapis.Library;

/// <summary>
/// A library item represents one local video file or one complete clear-BDMV
/// disc root; it never represents an individual .m2ts clip.
/// </summary>
public sealed record MediaLibraryItem(
    Guid Id,
    MediaLocatorKind Kind,
    string SourcePath,
    string Title,
    DateTimeOffset ImportedAtUtc)
{
    [JsonIgnore]
    public bool IsDisc => Kind == MediaLocatorKind.Directory;

    [JsonIgnore]
    public bool IsAvailable => IsDisc
        ? Directory.Exists(SourcePath) : File.Exists(SourcePath);

    [JsonIgnore]
    public string KindLabel => IsDisc ? "BLU-RAY DISC" : "LOCAL VIDEO";

    [JsonIgnore]
    public string AvailabilityLabel => IsAvailable
        ? (IsDisc ? "完整光碟資料夾" : "本機影片") : "原始路徑無法存取";

    public MediaLocator ToLocator() => new(Kind, SourcePath);
}
