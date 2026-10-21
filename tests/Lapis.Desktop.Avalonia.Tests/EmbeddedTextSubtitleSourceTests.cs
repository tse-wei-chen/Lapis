using System.Globalization;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class EmbeddedTextSubtitleSourceTests
{
    private const string Movie = "embedded-video.mkv";

    [Fact]
    public async Task OnlySubRipAndMovTextBecomeSelectableTextTracks()
    {
        var playback = new FakeEmbeddedPlayback
        {
            Tracks =
            [
                new(1, EmbeddedSubtitleTrackFormat.Text, "eng", "subrip"),
                new(2, EmbeddedSubtitleTrackFormat.Bitmap, "jpn", "hdmv_pgs_subtitle"),
                new(3, EmbeddedSubtitleTrackFormat.Text, "zho", "mov_text"),
                new(4, EmbeddedSubtitleTrackFormat.Text, "eng", "ass"),
                new(5, EmbeddedSubtitleTrackFormat.Unknown, "", "subrip")
            ]
        };
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var tracks = await source.DiscoverAsync(Movie, CancellationToken.None);

        Assert.Equal(2, tracks.Count);
        Assert.Equal(new[] { "1", "3" }, tracks.Select(t => t.TrackId));
        Assert.All(tracks, x =>
        {
            Assert.Equal(EmbeddedTextSubtitleSource.Id, x.SourceId);
            Assert.Equal(TextSubtitleSourceKind.EmbeddedText, x.Kind);
        });
        Assert.Contains("eng", tracks[0].DisplayName);
        Assert.Equal("zho", tracks[1].LanguageTag);
        Assert.Equal(0, playback.StartCalls);
    }

    [Fact]
    public async Task DecodedCuePagesUseExistingOverlapSafeTextTimeline()
    {
        var playback = OneTrack();
        playback.LoadingReads = 1;
        playback.Cues =
        [
            new(1_000_000_000, 4_000_000_000, "Long"),
            new(2_000_000_000, 3_000_000_000, "Short"),
            new(6_000_000_000, 8_000_000_000, "After")
        ];
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var descriptor = Assert.Single(await source.DiscoverAsync(
            Movie, CancellationToken.None));

        var track = await source.LoadAsync(Movie, descriptor, CancellationToken.None);
        Assert.Equal("Long", track.At(TimeSpan.FromSeconds(1.5)));
        Assert.Equal("Short", track.At(TimeSpan.FromSeconds(2.5)));
        Assert.Equal("Long", track.At(TimeSpan.FromSeconds(3.5)));
        Assert.Null(track.At(TimeSpan.FromSeconds(4)));
        Assert.Equal("After", track.At(TimeSpan.FromSeconds(6)));
        Assert.Null(track.At(TimeSpan.FromSeconds(8)));
        Assert.Equal(1, playback.StartCalls);
        Assert.True(playback.ReadCalls >= 2);
        Assert.Equal(new uint[] { 0 }, playback.ReadOffsets.Distinct());
    }

    [Fact]
    public async Task SeekEpochHidesOldTrackAndRefreshesSelectedStream()
    {
        var playback = OneTrack();
        playback.Cues = [new(1_000_000_000, 4_000_000_000, "English")];
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var selected = Assert.Single(await source.DiscoverAsync(Movie, CancellationToken.None));

        var before = await source.LoadAsync(Movie, selected, CancellationToken.None);
        Assert.Equal("English", before.At(TimeSpan.FromSeconds(2)));

        playback.AdvanceEpoch();
        Assert.Null(before.At(TimeSpan.FromSeconds(2)));
        var after = await source.LoadAsync(Movie, selected, CancellationToken.None);
        Assert.Equal("English", after.At(TimeSpan.FromSeconds(2)));
        Assert.Equal(2, playback.StartCalls);
        Assert.Equal(new ulong[] { 1, 2 }, playback.StartEpochs);
        playback.ClosePlayback();
        Assert.Null(after.At(TimeSpan.FromSeconds(2)));
    }

    [Fact]
    public async Task AGenerationThatChangesDuringStartCannotPublishOldText()
    {
        var playback = OneTrack();
        playback.Cues = [new(0, 1_000_000_000, "Old")];
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var selected = Assert.Single(await source.DiscoverAsync(Movie, CancellationToken.None));
        playback.AdvanceDuringStart = true;

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await source.LoadAsync(Movie, selected, CancellationToken.None));
        Assert.Equal(1, playback.StartCalls);
        Assert.Equal(0, playback.ReadCalls);
    }

    [Fact]
    public async Task OldNativeDllLeavesSidecarsAvailableAndNeverStartsDecode()
    {
        var playback = OneTrack();
        playback.MetadataUnsupported = true;
        var embedded = new EmbeddedTextSubtitleSource(playback, Movie);
        var catalog = new TextSubtitleSourceCatalog(
            [new SidecarSrtTextSubtitleSource(), embedded]);

        var tracks = await catalog.DiscoverAsync(Movie);
        Assert.Empty(tracks);
        Assert.Equal(0, playback.StartCalls);
        Assert.Null((await catalog.LoadPreferredAsync(Movie,
            CultureInfo.GetCultureInfo("zh-TW"))).At(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task RejectForgedTracksAndOtherMediaFilesAndCanceledRequests()
    {
        var playback = OneTrack();
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var selected = Assert.Single(await source.DiscoverAsync(Movie, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await source.LoadAsync("different.mkv", selected, CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await source.LoadAsync(Movie, selected with { TrackId = "0" },
                CancellationToken.None));
        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await source.LoadAsync(Movie, selected with {
                SourceId = SidecarSrtTextSubtitleSource.Id },
                CancellationToken.None));

        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await source.LoadAsync(Movie, selected, canceled.Token));
        Assert.Equal(0, playback.StartCalls);
    }

    [Fact]
    public async Task RejectProducerWithNoPageProgressInsteadOfPollingForever()
    {
        var playback = OneTrack();
        playback.ForceEmptyPageWithRemainingCues = true;
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var selected = Assert.Single(await source.DiscoverAsync(Movie, CancellationToken.None));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await source.LoadAsync(Movie, selected, CancellationToken.None));
        Assert.Equal(1, playback.ReadCalls);
    }

    [Fact]
    public async Task EnumerationOnlyDllDoesNotAdvertiseUnplayableEmbeddedChoice()
    {
        var playback = OneTrack();
        playback.CueApiUnsupported = true; // Metadata extension exists, decoder does not.
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        Assert.Empty(await source.DiscoverAsync(Movie, CancellationToken.None));
        Assert.Equal(0, playback.StartCalls);
        Assert.Equal(0, playback.ReadCalls);
    }

    [Fact]
    public async Task AutoPolicyDoesNotStartAnEmbeddedDecode()
    {
        var playback = OneTrack();
        var embedded = new EmbeddedTextSubtitleSource(playback, Movie);
        var catalog = new TextSubtitleSourceCatalog(
            [new SidecarSrtTextSubtitleSource(), embedded]);
        await catalog.LoadPreferredAsync(Movie, CultureInfo.GetCultureInfo("en-US"));
        Assert.Equal(0, playback.StartCalls);
        Assert.Equal(0, playback.ReadCalls);
    }

    [Fact]
    public async Task OffAndReplacementDisposeOnlyTheMatchingNativeSubtitleRequest()
    {
        var playback = OneTrack();
        playback.Cues = [new(0, 2_000_000_000, "Visible")];
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var descriptor = Assert.Single(await source.DiscoverAsync(
            Movie, CancellationToken.None));

        var first = await source.LoadAsync(Movie, descriptor, CancellationToken.None);
        var firstId = playback.ActiveRequestId;
        Assert.NotEqual(0UL, firstId);
        Assert.Equal("Visible", first.At(TimeSpan.FromSeconds(1)));

        var second = await source.LoadAsync(Movie, descriptor, CancellationToken.None);
        var secondId = playback.ActiveRequestId;
        Assert.NotEqual(firstId, secondId);
        await ((IAsyncDisposable)first).DisposeAsync();
        await ((IAsyncDisposable)first).DisposeAsync();
        Assert.Equal(secondId, playback.ActiveRequestId);
        Assert.Equal(new ulong[] { firstId }, playback.CancelledRequestIds);
        Assert.Null(first.At(TimeSpan.FromSeconds(1)));
        Assert.Equal("Visible", second.At(TimeSpan.FromSeconds(1)));

        // The Watch Off/Close path disposes the currently displayed track.
        await ((IAsyncDisposable)second).DisposeAsync();
        Assert.Equal(0UL, playback.ActiveRequestId);
        Assert.Equal(new ulong[] { firstId, secondId }, playback.CancelledRequestIds);
        Assert.Null(second.At(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task AbortedInFlightSelectionStopsNativeEvenWithCancelledUiToken()
    {
        var playback = OneTrack();
        playback.LoadingReads = int.MaxValue;
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var descriptor = Assert.Single(await source.DiscoverAsync(
            Movie, CancellationToken.None));
        using var cancellation = new CancellationTokenSource();

        var pending = source.LoadAsync(Movie, descriptor, cancellation.Token);
        await playback.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
        Assert.Equal(1, playback.StartCalls);
        Assert.Single(playback.CancelledRequestIds);
        Assert.Equal(0UL, playback.ActiveRequestId);
    }

    [Fact]
    public async Task FailedCuePageRetiresTheNativeRequest()
    {
        var playback = OneTrack();
        playback.ForceEmptyPageWithRemainingCues = true;
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var descriptor = Assert.Single(await source.DiscoverAsync(
            Movie, CancellationToken.None));

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            source.LoadAsync(Movie, descriptor, CancellationToken.None));
        Assert.Single(playback.CancelledRequestIds);
        Assert.Equal(0UL, playback.ActiveRequestId);
    }

    [Fact]
    public async Task OldCueApiWithoutCancelExtensionPreservesTextPlayback()
    {
        var playback = OneTrack();
        playback.CancellationSupported = false;
        playback.Cues = [new(0, 1_000_000_000, "Old DLL")];
        var source = new EmbeddedTextSubtitleSource(playback, Movie);
        var descriptor = Assert.Single(await source.DiscoverAsync(
            Movie, CancellationToken.None));
        var track = await source.LoadAsync(Movie, descriptor, CancellationToken.None);
        Assert.Equal("Old DLL", track.At(TimeSpan.FromMilliseconds(500)));
        Assert.Equal(1, playback.LegacyStartCalls);
        Assert.Empty(playback.CancelledRequestIds);
        await ((IAsyncDisposable)track).DisposeAsync();
        Assert.Empty(playback.CancelledRequestIds);
    }

    private static FakeEmbeddedPlayback OneTrack() => new()
    {
        Tracks = [new(1, EmbeddedSubtitleTrackFormat.Text, "eng", "subrip")]
    };

    private sealed class FakeEmbeddedPlayback : IEmbeddedSubtitlePlaybackService
    {
        private readonly PlaybackSessionId _id = PlaybackSessionId.New();
        public PlaybackSnapshot Snapshot { get; private set; }
        public IReadOnlyList<EmbeddedSubtitleTrack> Tracks { get; set; } = [];
        public List<EmbeddedTextSubtitleCue> Cues { get; set; } = [];
        public bool MetadataUnsupported { get; set; }
        public bool CueApiUnsupported { get; set; }
        public bool CancellationSupported { get; set; } = true;
        public int LegacyStartCalls { get; private set; }
        public ulong ActiveRequestId { get; private set; }
        public List<ulong> CancelledRequestIds { get; } = [];
        public TaskCompletionSource Started { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public bool AdvanceDuringStart { get; set; }
        public bool ForceEmptyPageWithRemainingCues { get; set; }
        public int LoadingReads { get; set; }
        public int StartCalls { get; private set; }
        public int ReadCalls { get; private set; }
        public List<ulong> StartEpochs { get; } = [];
        public List<uint> ReadOffsets { get; } = [];

        public FakeEmbeddedPlayback()
        {
            Snapshot = PlaybackSnapshot.CreateInitial(_id) with
            {
                Epoch = new PlaybackEpoch(1),
                Transport = TransportState.Playing
            };
        }

        public void AdvanceEpoch() =>
            Snapshot = Snapshot with { Epoch = new PlaybackEpoch(Snapshot.Epoch.Value + 1) };

        public void ClosePlayback() =>
            Snapshot = Snapshot with { Transport = TransportState.Closed };

        public Task<bool> SupportsEmbeddedTextSubtitleCuesAsync(
            PlaybackSnapshot expected, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(Snapshot.Epoch, expected.Epoch);
            return Task.FromResult(!CueApiUnsupported);
        }

        public Task<bool> SupportsEmbeddedTextSubtitleCancellationAsync(
            PlaybackSnapshot expected, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(Snapshot.Epoch, expected.Epoch);
            return Task.FromResult(CancellationSupported);
        }

        public Task StartEmbeddedTextSubtitleLoadWithIdAsync(
            uint streamIndex, ulong requestId, PlaybackSnapshot expected,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.NotEqual(0UL, requestId);
            ActiveRequestId = requestId;
            return RecordStart(streamIndex, expected);
        }

        public Task CancelEmbeddedTextSubtitleLoadAsync(
            ulong requestId, PlaybackSnapshot expected,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            // Old-epoch cleanup would be rejected by DesktopPlaybackHost.
            if (Snapshot.Epoch != expected.Epoch)
                throw new InvalidOperationException("Playback epoch has retired.");
            CancelledRequestIds.Add(requestId);
            if (ActiveRequestId == requestId) ActiveRequestId = 0;
            return Task.CompletedTask;
        }

        public Task<IReadOnlyList<EmbeddedSubtitleTrack>> ReadEmbeddedSubtitleTracksAsync(
            PlaybackSnapshot expected, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(Snapshot.Epoch, expected.Epoch);
            if (MetadataUnsupported)
                throw new NotSupportedException("old DLL");
            return Task.FromResult(Tracks);
        }

        public Task StartEmbeddedTextSubtitleLoadAsync(
            uint streamIndex, PlaybackSnapshot expected,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            LegacyStartCalls++;
            return RecordStart(streamIndex, expected);
        }

        private Task RecordStart(uint streamIndex, PlaybackSnapshot expected)
        {
            Assert.Equal(1U, streamIndex);
            Assert.Equal(Snapshot.Epoch, expected.Epoch);
            StartCalls++;
            StartEpochs.Add(expected.Epoch.Value);
            Started.TrySetResult();
            if (AdvanceDuringStart) AdvanceEpoch();
            return Task.CompletedTask;
        }

        public Task<EmbeddedTextSubtitleCuePage> ReadEmbeddedTextSubtitleCuesAsync(
            uint startIndex, PlaybackSnapshot expected,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(Snapshot.Epoch, expected.Epoch);
            ReadCalls++;
            ReadOffsets.Add(startIndex);

            if (LoadingReads-- > 0)
                return Task.FromResult(new EmbeddedTextSubtitleCuePage(
                    EmbeddedTextSubtitleLoadState.Loading, 0, []));
            if (ForceEmptyPageWithRemainingCues)
                return Task.FromResult(new EmbeddedTextSubtitleCuePage(
                    EmbeddedTextSubtitleLoadState.Ready, 1, []));
            var page = Cues.Skip(checked((int)startIndex)).Take(16).ToArray();
            return Task.FromResult(new EmbeddedTextSubtitleCuePage(
                EmbeddedTextSubtitleLoadState.Ready,
                checked((uint)Cues.Count), page));
        }
    }
}
