using Lapis.Contracts.Media;
using Lapis.Contracts.Playback;
using Lapis.Desktop.Hosting;
using Lapis.Runtime.Native;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// The one composition root for the Windows Native runtime. Instantiate lazily
/// so the Avalonia UI can start and report a missing DLL instead of crashing.
/// The existing UI-neutral DesktopPlaybackHost owns every playback command.
/// </summary>
public sealed class NativePlayerPlaybackService(Func<nint> getVideoHwnd)
    : IPlayerPlaybackService, IEmbeddedSubtitlePlaybackService
{
    private readonly PlaybackSnapshot _initial =
        PlaybackSnapshot.CreateInitial(PlaybackSessionId.New());
    private readonly Func<nint> _getVideoHwnd =
        getVideoHwnd ?? throw new ArgumentNullException(nameof(getVideoHwnd));
    private DesktopPlaybackHost? _host;
    private bool _disposed;

    public PlaybackSnapshot Snapshot => _host?.Snapshot ?? _initial;

    public async Task OpenAsync(MediaLocator locator, CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        var output = _getVideoHwnd(); // UI thread creates/owns the child HWND.
        if (output == 0)
            throw new InvalidOperationException("Video output window is not ready.");

        if (_host is null)
        {
            NativePlaybackRuntime runtime;
            try
            {
                runtime = new NativePlaybackRuntime();
            }
            catch (DllNotFoundException exception)
            {
                throw new InvalidOperationException(
                    "無法載入 Lapis 原生播放引擎或其 FFmpeg/libbluray 相依 DLL。" +
                    "請先編譯 Native Release，再重新建置 Avalonia，" +
                    "確定 Lapis.MediaRuntime.dll 與相依 DLL 一起部署於程式目錄。",
                    exception);
            }
            catch (BadImageFormatException exception)
            {
                throw new InvalidOperationException(
                    "Lapis 原生播放引擎與目前執行平台架構不相容；" +
                    "請確認 .NET 應用及所有 Native DLL 均為 x64。",
                    exception);
            }

            _host = new DesktopPlaybackHost(runtime, runtime);
        }

        await _host.OpenAndPlayAsync(locator, output, cancellationToken);
    }

    public Task ToggleAsync(CancellationToken cancellationToken) =>
        RequireHost().TogglePlayPauseAsync(cancellationToken);

    public Task SeekAsync(TimeSpan position, CancellationToken cancellationToken) =>
        RequireHost().SeekAsync(position, cancellationToken);

    public Task CloseAsync(CancellationToken cancellationToken) =>
        _host?.CloseMediaAsync(cancellationToken) ?? Task.CompletedTask;

    public IAsyncEnumerable<PlaybackSnapshot> ObserveSnapshotsAsync(
        CancellationToken cancellationToken) =>
        RequireHost().ObserveSnapshotsAsync(cancellationToken);

    public Task<bool> SupportsEmbeddedTextSubtitleCuesAsync(
        PlaybackSnapshot expected, CancellationToken cancellationToken) =>
        RequireHost().SupportsEmbeddedTextSubtitleCuesAsync(expected, cancellationToken);

    public Task<IReadOnlyList<EmbeddedSubtitleTrack>> ReadEmbeddedSubtitleTracksAsync(
        PlaybackSnapshot expected, CancellationToken cancellationToken) =>
        RequireHost().ReadEmbeddedSubtitleTracksAsync(expected, cancellationToken);

    public Task StartEmbeddedTextSubtitleLoadAsync(
        uint streamIndex, PlaybackSnapshot expected,
        CancellationToken cancellationToken) =>
        RequireHost().StartEmbeddedTextSubtitleLoadAsync(
            streamIndex, expected, cancellationToken);

    public Task<EmbeddedTextSubtitleCuePage> ReadEmbeddedTextSubtitleCuesAsync(
        uint startIndex, PlaybackSnapshot expected,
        CancellationToken cancellationToken) =>
        RequireHost().ReadEmbeddedTextSubtitleCuesAsync(
            startIndex, expected, cancellationToken);

    private DesktopPlaybackHost RequireHost() =>
        _host ?? throw new InvalidOperationException("No media has been opened.");

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        if (_host is { } host)
        {
            await host.DisposeAsync();
            _host = null;
        }
    }
}
