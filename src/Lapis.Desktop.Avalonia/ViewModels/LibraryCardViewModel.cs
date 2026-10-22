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
    private bool _started;
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
    public bool IsDisc => Item.IsDisc;
    public Bitmap? Poster => _poster;
    public bool HasPoster => _poster is not null;
    public bool ShowPlaceholder => _poster is null;
    public string PlaceholderGlyph => IsDisc ? "◈" : "▶";
    public string PlaceholderCaption => IsDisc ? "BLU-RAY" : "LOCAL VIDEO";
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
        // Discovery and all image I/O stay off the Avalonia UI thread.
        // Missing/corrupt artwork never prevents catalog browsing.
        _ = LoadPosterAsync();
    }

    private async Task LoadPosterAsync()
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
                            // Full-resolution covers must never be held per card.
                            return Bitmap.DecodeToWidth(stream, 400);
                        }
                        catch (Exception exception) when (exception is
                            IOException or UnauthorizedAccessException or
                            ArgumentException or InvalidOperationException)
                        {
                            // A damaged image is not a fatal library error;
                            // a lower-priority candidate may be valid.
                        }
                    }
                    return null;
                }, _lifetime.Token).ConfigureAwait(false);
            }
            finally
            {
                DecodeSlots.Release();
            }

            if (decoded is null) return;
            var image = decoded;
            decoded = null;
            Dispatcher.UIThread.Post(() =>
            {
                if (_disposed)
                {
                    image.Dispose();
                    return;
                }
                _poster?.Dispose();
                _poster = image;
                Notify(nameof(Poster));
                Notify(nameof(HasPoster));
                Notify(nameof(ShowPlaceholder));
            });
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
            // The card was removed while its cover was decoding.
        }
        catch (Exception)
        {
            // Unavailable drives and malformed local sources are nonfatal.
        }
        finally
        {
            decoded?.Dispose();
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
