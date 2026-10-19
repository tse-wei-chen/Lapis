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
        Assert.Equal("00:12 / 01:00", viewModel.PositionText);
        Assert.True(viewModel.CanTransport);
        Assert.True(viewModel.CanSeek);
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
    }

    private sealed class FakePlayer : IPlayerPlaybackService
    {
        public MediaLocator? LastLocator { get; private set; }
        public bool FailOpen { get; set; }
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
        public Task SeekAsync(TimeSpan position, CancellationToken cancellationToken) =>
            Task.CompletedTask;
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
