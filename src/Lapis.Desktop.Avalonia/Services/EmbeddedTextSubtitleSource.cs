using System.Diagnostics;
using System.Globalization;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// One Watch-session adapter to the optional embedded SubRip/mov_text port.
/// Sidecars retain Auto precedence; PGS and unsupported ASS never appear
/// as selectable text even if FFmpeg discovered them as metadata.
/// </summary>
public sealed class EmbeddedTextSubtitleSource : ITextSubtitleSource
{
    public const string Id = "embedded-ffmpeg-text";
    private readonly IEmbeddedSubtitlePlaybackService _playback;
    private readonly string _mediaPath;
    private readonly PlaybackSessionId _sessionId;
    private IReadOnlyDictionary<uint, EmbeddedSubtitleTrack> _discovered =
        new Dictionary<uint, EmbeddedSubtitleTrack>();

    public string SourceId => Id;

    public EmbeddedTextSubtitleSource(
        IEmbeddedSubtitlePlaybackService playback, string mediaPath)
    {
        _playback = playback ?? throw new ArgumentNullException(nameof(playback));
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        _mediaPath = Path.GetFullPath(mediaPath);
        _sessionId = playback.Snapshot.SessionId;
    }

    public async Task<IReadOnlyList<TextSubtitleDescriptor>> DiscoverAsync(
        string mediaPath, CancellationToken cancellationToken)
    {
        ValidatePath(mediaPath);
        cancellationToken.ThrowIfCancellationRequested();
        var captured = ActiveSnapshot();
        IReadOnlyList<EmbeddedSubtitleTrack> all;
        try
        {
            if (!await _playback.SupportsEmbeddedTextSubtitleCuesAsync(
                    captured, cancellationToken))
                return Array.Empty<TextSubtitleDescriptor>();
            EnsureCurrent(captured, cancellationToken);
            all = await _playback.ReadEmbeddedSubtitleTracksAsync(
                captured, cancellationToken);
        }
        catch (NotSupportedException)
        {
            // An older DLL still supports normal playback and sidecars.
            return Array.Empty<TextSubtitleDescriptor>();
        }

        EnsureCurrent(captured, cancellationToken);
        var available = all.Where(track =>
            track.Format == EmbeddedSubtitleTrackFormat.Text &&
            track.CodecName is "subrip" or "mov_text")
            .Take(64)
            .ToArray();
        var indexed = new Dictionary<uint, EmbeddedSubtitleTrack>();
        foreach (var track in available)
            indexed.TryAdd(track.StreamIndex, track);
        _discovered = indexed;

        return indexed.Values.Select(track =>
            new TextSubtitleDescriptor(
                Id, track.StreamIndex.ToString(CultureInfo.InvariantCulture),
                $"內嵌字幕 · {track.LanguageTag switch { "" => "未標記語言", _ => track.LanguageTag }} · {track.CodecName}",
                string.IsNullOrWhiteSpace(track.LanguageTag)
                    ? null : track.LanguageTag,
                TextSubtitleSourceKind.EmbeddedText)).ToArray();
    }

    public async Task<ITextSubtitleTrack> LoadAsync(
        string mediaPath, TextSubtitleDescriptor descriptor,
        CancellationToken cancellationToken)
    {
        ValidatePath(mediaPath);
        ArgumentNullException.ThrowIfNull(descriptor);
        cancellationToken.ThrowIfCancellationRequested();
        if (descriptor.SourceId != Id ||
            descriptor.Kind != TextSubtitleSourceKind.EmbeddedText ||
            !uint.TryParse(descriptor.TrackId, NumberStyles.None,
                CultureInfo.InvariantCulture, out var streamIndex) ||
            !_discovered.ContainsKey(streamIndex))
            throw new ArgumentException(
                "Unknown or unplayable embedded text subtitle track.", nameof(descriptor));

        var captured = ActiveSnapshot();
        await _playback.StartEmbeddedTextSubtitleLoadAsync(
            streamIndex, captured, cancellationToken);
        EnsureCurrent(captured, cancellationToken);

        var start = Stopwatch.GetTimestamp();
        const int maxCues = 4096;
        var cues = new List<SidecarSubtitleTrack.Cue>();
        uint nextIndex = 0;

        while (true)
        {
            EnsureCurrent(captured, cancellationToken);
            if (Stopwatch.GetElapsedTime(start) > TimeSpan.FromMinutes(1))
                throw new TimeoutException("Embedded subtitle decoding did not complete.");

            var page = await _playback.ReadEmbeddedTextSubtitleCuesAsync(
                nextIndex, captured, cancellationToken);
            EnsureCurrent(captured, cancellationToken);

            switch (page.State)
            {
                case EmbeddedTextSubtitleLoadState.Loading:
                    await Task.Delay(75, cancellationToken);
                    continue;
                case EmbeddedTextSubtitleLoadState.Failed:
                case EmbeddedTextSubtitleLoadState.LimitExceeded:
                case EmbeddedTextSubtitleLoadState.Cancelled:
                    throw new InvalidOperationException(
                        $"Embedded subtitle decode finished with {page.State}.");
                case EmbeddedTextSubtitleLoadState.Ready:
                    break;
                default:
                    throw new InvalidOperationException("Invalid native subtitle state.");
            }

            if (page.Total > maxCues ||
                page.Cues.Count > 16 ||
                (ulong)nextIndex + (ulong)page.Cues.Count > page.Total)
                throw new InvalidOperationException("Invalid embedded subtitle cue count.");

            foreach (var cue in page.Cues)
            {
                if (cue.StartNanoseconds < 0 ||
                    cue.EndNanoseconds <= cue.StartNanoseconds)
                    throw new InvalidOperationException("Invalid embedded subtitle cue time.");

                var beginning = TimeSpan.FromTicks(cue.StartNanoseconds / 100);
                var ending = TimeSpan.FromTicks(cue.EndNanoseconds / 100);
                if (ending <= beginning) continue;
                cues.Add(new SidecarSubtitleTrack.Cue(beginning, ending, cue.Text));
            }

            // A malformed producer must never spin indefinitely on an empty
            // page while claiming additional results are available.
            nextIndex += checked((uint)page.Cues.Count);
            if (nextIndex == page.Total) break;
            if (page.Cues.Count == 0)
                throw new InvalidOperationException(
                    "Embedded subtitle producer stopped making page progress.");
        }

        EnsureCurrent(captured, cancellationToken);
        return new EpochGuardedTrack(
            SidecarSubtitleTrack.FromCues(cues), _playback, captured);
    }

    public Task<ITextSubtitleTrack> LoadPreferredAsync(
        string mediaPath, CultureInfo culture, CancellationToken cancellationToken)
    {
        // Auto belongs to the first (SRT) provider and never implicitly
        // starts a native decode worker or AI translation pipeline.
        ValidatePath(mediaPath);
        cancellationToken.ThrowIfCancellationRequested();
        return Task.FromResult<ITextSubtitleTrack>(SidecarSubtitleTrack.Empty);
    }

    private void ValidatePath(string mediaPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(mediaPath);
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        if (!string.Equals(Path.GetFullPath(mediaPath), _mediaPath, comparison))
            throw new ArgumentException(
                "An embedded subtitle track belongs to its original media file.",
                nameof(mediaPath));
    }

    private PlaybackSnapshot ActiveSnapshot()
    {
        var snapshot = _playback.Snapshot;
        if (snapshot.SessionId != _sessionId ||
            snapshot.Transport is not (TransportState.Ready or TransportState.Playing
                or TransportState.Paused or TransportState.Ended))
            throw new OperationCanceledException(
                "The original Watch playback session is no longer active.");
        return snapshot;
    }

    private void EnsureCurrent(
        PlaybackSnapshot expected, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var snapshot = _playback.Snapshot;
        if (snapshot.SessionId != expected.SessionId ||
            snapshot.Epoch != expected.Epoch ||
            snapshot.Transport is not (TransportState.Ready or TransportState.Playing
                or TransportState.Paused or TransportState.Ended))
            throw new OperationCanceledException(
                "The embedded subtitle playback generation has retired.");
    }

    private sealed class EpochGuardedTrack(
        ITextSubtitleTrack inner, IEmbeddedSubtitlePlaybackService playback,
        PlaybackSnapshot origin) : ITextSubtitleTrack
    {
        public string? At(TimeSpan position)
        {
            var now = playback.Snapshot;
            return now.SessionId == origin.SessionId &&
                   now.Epoch == origin.Epoch &&
                   now.Transport is (TransportState.Ready or TransportState.Playing
                       or TransportState.Paused or TransportState.Ended)
                ? inner.At(position) : null;
        }
    }
}
