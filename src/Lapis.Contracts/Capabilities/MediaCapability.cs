namespace Lapis.Contracts.Capabilities;

[Flags]
public enum MediaCapability : ulong
{
    None = 0,
    MenuNavigation = 1UL << 0,
    MultipleAngles = 1UL << 1,
    Chapters = 1UL << 2,
    HdrOutput = 1UL << 3,
    BitstreamOutput = 1UL << 4,
    BitmapSubtitles = 1UL << 5,
    AiTranslation = 1UL << 6,
    RealtimeAsr = 1UL << 7,
    OfflineAsr = 1UL << 8,
    DiscNavigation = 1UL << 9
}
