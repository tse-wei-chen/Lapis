using System.Runtime.CompilerServices;
using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Avalonia.Services;
using Lapis.Desktop.Avalonia.ViewModels;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class PlayerViewModelTests
{
    [Fact]
    public async Task OpenKeepsTypedLocatorAndDisplaysActorSnapshot()
    {
        var fake = new FakePlayer();
        await using var viewModel = new PlayerViewModel(fake);
        var locator = new MediaLocator(MediaLocatorKind.Directory, @"C:\TestDisc\BDMV");
        await viewModel.OpenAsync(locator);

        Assert.Same(locator, fake.LastLocator);
        Assert.Equal("BDMV · BDMV", viewModel.MediaTitle);
        Assert.Equal("Playing", viewModel.TransportText);
        Assert.True(viewModel.IsPlaying);
        Assert.False(viewModel.IsNotPlaying);
        Assert.Equal("00:12 / 01:00", viewModel.PositionText);
        Assert.True(viewModel.CanTransport);
        Assert.True(viewModel.CanSeek);
        Assert.True(viewModel.CanScrub);
        Assert.Equal(12, viewModel.SeekPositionSeconds);
        Assert.Equal(60, viewModel.SeekDurationSeconds);
    }

    [Fact]
    public async Task FileOpenErrorIsReportedWithoutClaimingPlayback()
    {
        var fake = new FakePlayer { FailOpen = true };
        await using var viewModel = new PlayerViewModel(fake);
        await viewModel.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"));

        Assert.Contains("無法執行", viewModel.StatusText);
        Assert.False(viewModel.CanTransport);
        Assert.Equal("尚未開啟影片", viewModel.MediaTitle);
    }

    [Fact]
    public async Task EndedPlaybackAllowsReplayButNotSeeking()
    {
        var fake = new FakePlayer { OpenTransport = TransportState.Ended };
        await using var viewModel = new PlayerViewModel(fake);
        await viewModel.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"));

        Assert.True(viewModel.CanTransport);
        Assert.False(viewModel.CanSeek);
        Assert.Equal("重播", viewModel.PlayPauseLabel);
        Assert.False(viewModel.IsPlaying);
        Assert.True(viewModel.IsNotPlaying);
    }

    [Fact]
    public async Task DragPreviewsTimeAndCommitsExactlyOneSeekAtRelease()
    {
        var fake = new FakePlayer();
        await using var viewModel = new PlayerViewModel(fake);
        await viewModel.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"));
        viewModel.BeginScrub();
        viewModel.SeekPositionSeconds = 26.5;
        viewModel.SeekPositionSeconds = 72; // Clamp to known duration.
        Assert.Equal(60, viewModel.SeekPositionSeconds);
        Assert.Equal("01:00 / 01:00", viewModel.PositionText);
        Assert.Equal(0, fake.SeekCount);

        await viewModel.CommitScrubAsync();
        await viewModel.CommitScrubAsync(); // A second release is a no-op.
        Assert.Equal(1, fake.SeekCount);
        Assert.Equal(TimeSpan.FromSeconds(60), fake.LastSeek);
        Assert.Equal(60, viewModel.SeekPositionSeconds);
        Assert.True(viewModel.CanSeek);
    }

    [Theory]
    [InlineData(true, 22)]
    [InlineData(false, 2)]
    public async Task RelativeSeekCommandsWorkInsideTheBusyCommandGate(
        bool forward, int expectedSeconds)
    {
        var fake = new FakePlayer();
        await using var viewModel = new PlayerViewModel(fake);
        await viewModel.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"));

        if (forward) viewModel.ForwardCommand.Execute(null);
        else viewModel.BackCommand.Execute(null);

        var applied = await fake.SeekObserved.Task.WaitAsync(TimeSpan.FromSeconds(3));
        Assert.Equal(TimeSpan.FromSeconds(expectedSeconds), applied);
        Assert.Equal(1, fake.SeekCount);
    }

    [Fact]
    public async Task RepeatedForwardInputDoesNotQueueSeeksWhileOneIsInFlight()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var fake = new FakePlayer { PendingSeekRelease = release.Task };
        await using var viewModel = new PlayerViewModel(fake);
        await viewModel.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"));

        viewModel.ForwardCommand.Execute(null);
        await fake.SeekObserved.Task.WaitAsync(TimeSpan.FromSeconds(3));
        for (var count = 0; count < 10; ++count)
            viewModel.ForwardCommand.Execute(null);

        release.TrySetResult();
        // Close is sequenced after any mistakenly enqueued seeks.
        await viewModel.CloseAsync();
        Assert.Equal(1, fake.SeekCount);
    }

    [Fact]
    public async Task NewEpochInvalidatesOldScrubAndCancellationDoesNotSeek()
    {
        var fake = new FakePlayer();
        await using var viewModel = new PlayerViewModel(fake);
        await viewModel.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"));

        viewModel.BeginScrub();
        viewModel.SeekPositionSeconds = 44;
        fake.AdvanceEpoch();
        await viewModel.CommitScrubAsync();
        Assert.Equal(0, fake.SeekCount);
        Assert.Equal(12, viewModel.SeekPositionSeconds);

        viewModel.BeginScrub();
        viewModel.SeekPositionSeconds = 33;
        viewModel.CancelScrub();
        Assert.Equal(12, viewModel.SeekPositionSeconds);
        Assert.Equal(0, fake.SeekCount);
    }

    [Fact]
    public async Task EndOfCommandRepublishesEnabledSeekState()
    {
        var fake = new FakePlayer();
        await using var viewModel = new PlayerViewModel(fake);
        var changes = new List<bool>();
        viewModel.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(PlayerViewModel.CanSeek))
                changes.Add(viewModel.CanSeek);
        };

        await viewModel.OpenAsync(new MediaLocator(MediaLocatorKind.File, "movie.mkv"));
        Assert.NotEmpty(changes);
        Assert.True(changes[^1]);
    }

    private sealed class FakePlayer : IPlayerPlaybackService
    {
        public MediaLocator? LastLocator { get; private set; }
        public bool FailOpen { get; set; }
        public int SeekCount { get; private set; }
        public TimeSpan? LastSeek { get; private set; }
        public TaskCompletionSource<TimeSpan> SeekObserved { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task? PendingSeekRelease { get; init; }
        public void AdvanceEpoch() =>
            Snapshot = Snapshot with { Epoch = new PlaybackEpoch(Snapshot.Epoch.Value + 1) };
        public TransportState OpenTransport { get; set; } = TransportState.Playing;
        public PlaybackSnapshot Snapshot { get; private set; } =
            PlaybackSnapshot.CreateInitial(PlaybackSessionId.New());

        public Task OpenAsync(MediaLocator locator, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (FailOpen) throw new InvalidOperationException("failed to open");
            LastLocator = locator;
            Snapshot = Snapshot with
            {
                Epoch = new PlaybackEpoch(1),
                Transport = OpenTransport,
                Position = Lapis.Contracts.Time.MediaTime.FromSeconds(12),
                Duration = Lapis.Contracts.Time.MediaTime.FromSeconds(60)
            };
            return Task.CompletedTask;
        }

        public Task ToggleAsync(CancellationToken cancellationToken) =>
            Task.CompletedTask;
        public Task SeekAsync(TimeSpan position, CancellationToken cancellationToken)
        {
            SeekCount++;
            LastSeek = position;
            SeekObserved.TrySetResult(position);
            Snapshot = Snapshot with
            {
                Epoch = new PlaybackEpoch(Snapshot.Epoch.Value + 1),
                Position = Lapis.Contracts.Time.MediaTime.FromTimeSpan(position)
            };
            return PendingSeekRelease ?? Task.CompletedTask;
        }
        public Task CloseAsync(CancellationToken cancellationToken) =>
            Task.CompletedTask;
        public async IAsyncEnumerable<PlaybackSnapshot> ObserveSnapshotsAsync(
            [EnumeratorCancellation] CancellationToken cancellationToken)
        {
            await Task.CompletedTask;
            yield break;
        }
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
