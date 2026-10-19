using Lapis.Contracts.Media;
using Lapis.Contracts.Time;

namespace Lapis.Application.Playback;

public abstract record PlaybackCommand;

public sealed record OpenMediaCommand(MediaLocator Locator) : PlaybackCommand;

public sealed record PlayCommand : PlaybackCommand;

public sealed record PauseCommand : PlaybackCommand;

public sealed record SeekCommand(MediaTime Position) : PlaybackCommand;

// Resolves the current title's chapter in the serialized actor, then uses
// precisely the same playback-epoch-advancing seek as absolute positioning.
public sealed record SeekChapterCommand(uint ChapterId) : PlaybackCommand;

public sealed record CloseCommand : PlaybackCommand;
