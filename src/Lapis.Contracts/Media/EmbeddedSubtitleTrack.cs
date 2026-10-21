namespace Lapis.Contracts.Media;

/// <summary>
/// Metadata-only categorization. A discovered track is not necessarily
/// supported by the decoder or presentation backend.
/// </summary>
public enum EmbeddedSubtitleTrackFormat
{
    Unknown = 0,
    Text = 1,
    Bitmap = 2
}

/// <summary>
/// Owned metadata for one in-container subtitle stream. StreamIndex is the
/// actual container stream index, never an ordinal in the subtitle list.
/// </summary>
public sealed record EmbeddedSubtitleTrack(
    uint StreamIndex,
    EmbeddedSubtitleTrackFormat Format,
    string LanguageTag,
    string CodecName);
