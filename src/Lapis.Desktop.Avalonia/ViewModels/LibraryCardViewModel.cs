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

    public void EnsurePosterLoaded()
    {
        if (_started || _disposed) return;
        _started = true;
        var path = DiscArtworkLocator.FindPreferredArtwork(Item);
        if (path is not null)
            _ = LoadPosterAsync(path);
    }

    private async Task LoadPosterAsync(string path)
    {
        Bitmap? decoded = null;
        try
        {
            await DecodeSlots.WaitAsync(_lifetime.Token).ConfigureAwait(false);
            try
            {
                decoded = await Task.Run(() =>
                {
                    using var stream = new FileStream(path, FileMode.Open,
                        FileAccess.Read, FileShare.ReadWrite);
                    if (stream.Length is <= 0 or > DiscArtworkLocator.MaxSourceBytes)
                        return null;
                    // Never hold a full-resolution disc jacket in each UI tile.
                    return Bitmap.DecodeToWidth(stream, 400);
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
        catch (Exception)
        {
            // Broken/unmounted disc art is never allowed to prevent library
            // browsing. The stable placeholder remains visible.
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
