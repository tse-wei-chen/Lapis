using Lapis.Contracts.Media;
using Lapis.Contracts.Time;

namespace Lapis.Application.Playback;

public abstract record PlaybackCommand;

public sealed record OpenMediaCommand(MediaLocator Locator) : PlaybackCommand;

public sealed record PlayCommand : PlaybackCommand;

public sealed record PauseCommand : PlaybackCommand;

public sealed record SeekCommand(MediaTime Position) : PlaybackCommand;

public sealed record CloseCommand : PlaybackCommand;
