using System.ComponentModel;
using System.Runtime.CompilerServices;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Lapis.Library;

namespace Lapis.Desktop.Avalonia.ViewModels;

/// <summary>
/// Per-library-tile presentation state. Image decoding is bounded and off the
/// UI thread; decoded bitmaps live only for the card lifetime. Native playback
/// and the versioned catalog have no dependency on Avalonia graphics types.
/// </summary>
public sealed class LibraryCardViewModel : INotifyPropertyChanged, IDisposable
{
    private static readonly SemaphoreSlim DecodeSlots = new(2, 2);
    private readonly CancellationTokenSource _lifetime = new();
    private Bitmap? _poster;
    private WatchProgress? _watchProgress;
    private WatchProgress? _watchHistory;
    private bool _favorite;
    private bool _started;
    private bool _posterLoading;
    private string _posterRefreshStatus = "";
    private bool _disposed;

    public LibraryCardViewModel(MediaLibraryItem item) =>
        Item = item ?? throw new ArgumentNullException(nameof(item));

    public event PropertyChangedEventHandler? PropertyChanged;

    public MediaLibraryItem Item { get; }
    public string Title => Item.Title;
    public string KindLabel => Item.KindLabel;
    public string AvailabilityLabel => Item.AvailabilityLabel;
    public string SourcePath => Item.SourcePath;
    public string ImportedAtText => Item.ImportedAtUtc.ToLocalTime().ToString("yyyy/MM/dd");
    public bool IsAvailable => Item.IsAvailable;
    public bool NeedsSourceRepair => !IsAvailable;

    /// <summary>Refresh availability after unplugging a drive while the app is open.</summary>
    public void RefreshSourceAvailability()
    {
        if (_disposed) return;
        Notify(nameof(IsAvailable));
        Notify(nameof(NeedsSourceRepair));
        Notify(nameof(AvailabilityLabel));
        Notify(nameof(CanResume));
        Notify(nameof(PlayActionLabel));
    }

    public bool IsDisc => Item.IsDisc;
    public Bitmap? Poster => _poster;
    public bool HasPoster => _poster is not null;
    public bool ShowPlaceholder => _poster is null;
    public bool CanRefreshPoster => !_disposed && !_posterLoading;
    public string PosterRefreshStatus => _posterRefreshStatus;
    public string PlaceholderGlyph => IsDisc ? "◈" : "▶";
    public string PlaceholderCaption => IsDisc ? "BLU-RAY" : "LOCAL VIDEO";
    public bool IsFavorite => _favorite;
    public string FavoriteActionLabel => _favorite ? "★  移出我的最愛" : "☆  加入我的最愛";

    internal void SetFavorite(bool favorite)
    {
        if (_favorite == favorite) return;
        _favorite = favorite;
        Notify(nameof(IsFavorite));
        Notify(nameof(FavoriteActionLabel));
    }
    public bool CanResume => !IsDisc && IsAvailable && _watchProgress?.IsResumable == true;
    public double ResumePositionSeconds => CanResume ? _watchProgress!.PositionSeconds : 0;
    public string PlayActionLabel => CanResume ? "▶  繼續播放" : "▶  開始播放";
    public string ProgressLabel => CanResume
        ? $"從 {FormatPosition(ResumePositionSeconds)} 繼續觀看" : "";
    public double ProgressPercent => CanResume
        ? 100 * _watchProgress!.PositionSeconds / _watchProgress.DurationSeconds : 0;
    public DateTimeOffset LastWatchedUtc => _watchProgress?.UpdatedAtUtc ?? DateTimeOffset.MinValue;

    public bool HasWatchHistory => !IsDisc && _watchHistory?.IsHistoryEligible == true;
    public bool IsWatched => HasWatchHistory && _watchHistory!.IsCompleted;
    public string HistoryLabel => !HasWatchHistory ? "" :
        IsWatched ? "✓ 已看完" : $"觀看至 {FormatPosition(_watchHistory!.PositionSeconds)}";
    public string LastWatchedLabel => !HasWatchHistory ? "" :
        $"最近觀看：{_watchHistory!.UpdatedAtUtc.ToLocalTime():yyyy/MM/dd HH:mm}";
    public DateTimeOffset HistoryUpdatedAtUtc => _watchHistory?.UpdatedAtUtc ?? DateTimeOffset.MinValue;

    internal void SetWatchHistory(WatchProgress? history)
    {
        if (Equals(_watchHistory, history)) return;
        _watchHistory = history;
        Notify(nameof(HasWatchHistory));
        Notify(nameof(IsWatched));
        Notify(nameof(HistoryLabel));
        Notify(nameof(LastWatchedLabel));
        Notify(nameof(HistoryUpdatedAtUtc));
    }

    internal void SetWatchProgress(WatchProgress? progress)
    {
        if (Equals(_watchProgress, progress)) return;
        _watchProgress = progress;
        Notify(nameof(CanResume));
        Notify(nameof(ResumePositionSeconds));
        Notify(nameof(PlayActionLabel));
        Notify(nameof(ProgressLabel));
        Notify(nameof(ProgressPercent));
        Notify(nameof(LastWatchedUtc));
    }

    private static string FormatPosition(double seconds)
    {
        var time = TimeSpan.FromSeconds(Math.Max(0, seconds));
        return time.TotalHours >= 1
            ? $"{(long)time.TotalHours:00}:{time.Minutes:00}:{time.Seconds:00}"
            : $"{(long)time.TotalMinutes:00}:{time.Seconds:00}";
    }

    public void EnsurePosterLoaded()
    {
        if (_started || _disposed) return;
        _started = true;
        StartPosterScan(manual: false);
    }

    /// <summary>
    /// Re-discover local sidecars without changing the catalog or restarting
    /// playback. Calls are serialized per card and capped globally at two
    /// concurrent image decodes. Can safely run after a prior missing cover.
    /// </summary>
    public void RefreshPoster()
    {
        if (_disposed || _posterLoading) return;
        _started = true;
        StartPosterScan(manual: true);
    }

    private void StartPosterScan(bool manual)
    {
        _posterLoading = true;
        _posterRefreshStatus = manual ? "正在重新掃描本機封面…" : "";
        Notify(nameof(CanRefreshPoster));
        Notify(nameof(PosterRefreshStatus));
        _ = LoadPosterAsync(manual);
    }

    private async Task LoadPosterAsync(bool manual)
    {
        Bitmap? decoded = null;
        try
        {
            await DecodeSlots.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            try
            {
                decoded = await Task.Run(() =>
                {
                    foreach (var path in LocalArtworkLocator.FindCandidates(Item))
                    {
                        _lifetime.Token.ThrowIfCancellationRequested();
                        try
                        {
                            using var stream = new FileStream(path, FileMode.Open,
                                FileAccess.Read, FileShare.ReadWrite);
                            if (stream.Length is <= 0 or > LocalArtworkLocator.MaxSourceBytes)
                                continue;
                            // Never hold a full-resolution disc cover in each card.
                            return Bitmap.DecodeToWidth(stream, 400);
                        }
                        catch (Exception exception) when (exception is
                            IOException or UnauthorizedAccessException or
                            ArgumentException or InvalidOperationException)
                        {
                            // A damaged sidecar cannot prevent trying the next.
                        }
                    }
                    return null;
                }, _lifetime.Token).ConfigureAwait(false);
            }
            finally
            {
                DecodeSlots.Release();
            }
        }
        catch (OperationCanceledException)
        {
            // Card removed while scanning; do not publish stale artwork.
        }
        catch (Exception)
        {
            // Missing media drives or invalid image data are nonfatal.
        }
        finally
        {
            var next = decoded;
            Dispatcher.UIThread.Post(() =>
            {
                if (_disposed)
                {
                    next?.Dispose();
                    return;
                }

                // Null means the cover was removed or no usable image was found.
                // It must also clear the old poster rather than retaining stale art.
                var previous = _poster;
                _poster = next;
                previous?.Dispose();
                _posterLoading = false;
                _posterRefreshStatus = manual
                    ? next is null ? "未找到可用封面；請確認圖片名稱與格式。" : "封面已更新。"
                    : "";
                Notify(nameof(Poster));
                Notify(nameof(HasPoster));
                Notify(nameof(ShowPlaceholder));
                Notify(nameof(CanRefreshPoster));
                Notify(nameof(PosterRefreshStatus));
            });
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _lifetime.Cancel();
        _lifetime.Dispose();
        _poster?.Dispose();
        _poster = null;
    }

    private void Notify([CallerMemberName] string property = "") =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
}
