using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using FlaUI.Core;
using FlaUI.Core.AutomationElements;
using FlaUI.Core.Input;
using FlaUI.Core.WindowsAPI;
using FlaUI.UIA3;
using Lapis.Contracts.Media;
using Lapis.Library;
using Xunit;
using UIApp = FlaUI.Core.Application;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace Lapis.Desktop.E2E;

/// <summary>
/// Launches the actual Lapis executable as an independent process.
/// No in-process Avalonia test host, fake controls, or mocked D3D11 runtime.
/// </summary>
public sealed class WindowsDesktopJourneys
{
    [Fact]
    public async Task BigPicture_Manage_And_Back_UseActualDesktopAutomation()
    {
        using var session = await DesktopSession.CreateAsync();
        session.Start();
        var main = session.MainWindow();

        var play = session.WaitFor(main, "Lapis.Library.PlayFeatured");
        Assert.NotNull(play);
        session.Snapshot("01-library");
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.Manage"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.Home"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.Add"));

        // A poster/info action opens a cinematic detail surface; it is not an
        // accidental immediate player launch.
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.DetailPlay"));
        session.Click(session.WaitFor(main, "Lapis.Library.DetailClose"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));

        // UIA Invoke activates the real product Button, not the ViewModel.
        session.Click(session.WaitFor(main, "Lapis.Library.Manage"));
        // Windows UIA often omits non-interactive Grid containers. Verify
        // a real focused/interactive management navigation button instead.
        session.WaitFor(main, "Lapis.Library.ManagementAll");
        session.Snapshot("02-management");
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.Home"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.Add"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.ManagementBack"));
        Assert.Null(main.FindFirstDescendant(cf => cf.ByAutomationId(
            "Lapis.Library.PlayFeatured")));

        session.Click(session.WaitFor(main, "Lapis.Library.ManagementBack"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));
        session.Click(session.WaitFor(main, "Lapis.Library.Manage"));
        session.WaitFor(main, "Lapis.Library.ManagementAll");

        main.Focus();
        Keyboard.Press(VirtualKeyShort.ESCAPE);
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));
        session.Snapshot("03-library-restored");
    }

    [Fact]
    public async Task ExistingWatchBookmarkAppearsAsResumeActionInMovieDetails()
    {
        using var session = await DesktopSession.CreateAsync();
        await session.SeedWatchBookmarkAsync();
        session.Start();
        var main = session.MainWindow();

        // The bookmark was written to the isolated sidecar BEFORE launch.
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.DetailResume"));
        session.Click(session.WaitFor(main, "Lapis.Library.DetailClose"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));
    }

    [Fact]
    public async Task CompletedMovieRemainsInWatchHistoryAndCanBeForgotten()
    {
        using var session = await DesktopSession.CreateAsync();
        await session.SeedFinishedWatchAsync();
        session.Start();
        var main = session.MainWindow();

        session.Click(session.WaitFor(main, "Lapis.Library.HistoryTab"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.DetailForget"));
        Assert.Null(main.FindFirstDescendant(cf =>
            cf.ByAutomationId("Lapis.Library.DetailResume")));
        session.Snapshot("07-watch-history-completed");
        session.Click(session.WaitFor(main, "Lapis.Library.DetailForget"));

        // No media file was removed; the home page is still playable.
        session.Click(session.WaitFor(main, "Lapis.Library.Home"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));
    }

    [Fact]
    public async Task FavoriteMovieCanBePinnedFilteredAndUnpinnedInActualExe()
    {
        using var session = await DesktopSession.CreateAsync();
        session.Start();
        var main = session.MainWindow();
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        session.Click(session.WaitFor(main, "Lapis.Library.DetailFavorite"));
        session.WaitForName(main, "Lapis.Library.DetailFavorite", "移出我的最愛");
        session.WaitForFavoriteCount(1);

        session.Click(session.WaitFor(main, "Lapis.Library.FavoritesTab"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        session.WaitForName(main, "Lapis.Library.DetailFavorite", "移出我的最愛");
        session.Click(session.WaitFor(main, "Lapis.Library.DetailFavorite"));
        session.WaitForFavoriteCount(0);
        session.Snapshot("09-favorites-unpinned");
        session.Click(session.WaitFor(main, "Lapis.Library.Home"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));
        Assert.True(session.ProcessIsRunning);
    }

    [Fact]
    public async Task MissingMovieOffersSourceRepairInsteadOfBrokenPlayback()
    {
        using var session = await DesktopSession.CreateAsync();
        var moved = session.MoveMovieOffline();
        session.Start();
        var main = session.MainWindow();
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.DetailRepairSource"));
        Assert.False(session.WaitFor(main, "Lapis.Library.DetailPlay").IsEnabled);
        Assert.True(File.Exists(moved));
        session.Snapshot("10-missing-source-repair");
        session.Click(session.WaitFor(main, "Lapis.Library.DetailClose"));
        Assert.True(session.ProcessIsRunning);
    }

    [Fact]
    public async Task RenameMovieTitlePersistsAcrossCatalogReloadInActualExe()
    {
        using var session = await DesktopSession.CreateAsync();
        await session.SeedWatchBookmarkAsync();
        session.Start();
        var main = session.MainWindow();

        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        session.Click(session.WaitFor(main, "Lapis.Library.DetailRename"));
        var input = session.WaitFor(main, "Lapis.Library.RenameInput").AsTextBox();
        input.Text = "My Home Cinema";
        session.Click(session.WaitFor(main, "Lapis.Library.RenameSave"));
        session.WaitForCatalogTitle("My Home Cinema");

        // The detail sheet closes cleanly and the original media can still
        // be selected with its bookmark attached to the unchanged item ID.
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.DetailResume"));
        session.Snapshot("11-renamed-movie");
        session.Click(session.WaitFor(main, "Lapis.Library.DetailClose"));
        Assert.True(session.ProcessIsRunning);
    }

    [Fact]
    public async Task BigPictureRemoveRequiresConfirmationKeepsMediaAndCleansSidecars()
    {
        using var session = await DesktopSession.CreateAsync();
        await session.SeedWatchBookmarkAsync();
        await session.SeedFavoriteAsync();
        session.Start();
        var main = session.MainWindow();

        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));
        session.Click(session.WaitFor(main, "Lapis.Library.DetailRemove"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.RemoveCancel"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.RemoveConfirm"));

        // Cancellation must leave media, catalog and watch/favorite metadata intact.
        session.Click(session.WaitFor(main, "Lapis.Library.RemoveCancel"));
        Assert.True(session.MediaFileExists);
        session.WaitForCatalogCount(1);
        session.WaitForFavoriteCount(1);
        session.WaitForWatchCount(1);

        session.Click(session.WaitFor(main, "Lapis.Library.DetailRemove"));
        session.Snapshot("12-confirm-library-remove");
        session.Click(session.WaitFor(main, "Lapis.Library.RemoveConfirm"));
        session.WaitForCatalogCount(0);
        session.WaitForFavoriteCount(0);
        session.WaitForWatchCount(0);
        Assert.True(session.MediaFileExists); // Original MP4 was never deleted.
        Assert.True(session.ProcessIsRunning);
        Assert.Null(main.FindFirstDescendant(cf =>
            cf.ByAutomationId("Lapis.Library.PlayFeatured")));
    }

    [Fact]
    public async Task FilmCoverRefreshFindsNewSidecarAndClearsRemovedPoster()
    {
        using var session = await DesktopSession.CreateAsync();
        session.Start();
        var main = session.MainWindow();
        session.Click(session.WaitFor(main, "Lapis.Library.FeaturedDetails"));

        // Initial catalog scan has completed before the refresh button enables.
        var cover = session.AddPosterFixture();
        session.Click(session.WaitForEnabled(main, "Lapis.Library.DetailRefreshPoster"));
        session.WaitForName(main, "Lapis.Library.PosterRefreshStatus", "封面已更新");
        session.Snapshot("08-cover-reloaded");

        File.Delete(cover);
        session.Click(session.WaitForEnabled(main, "Lapis.Library.DetailRefreshPoster"));
        session.WaitForName(main, "Lapis.Library.PosterRefreshStatus", "未找到可用封面");
        session.Click(session.WaitFor(main, "Lapis.Library.DetailClose"));
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));
    }

    [Fact]
    public async Task Movie_Watch_Subtitles_And_Back_RoundTripThroughRealExe()
    {
        using var session = await DesktopSession.CreateAsync();
        session.Start();
        var main = session.MainWindow();
        session.Click(session.WaitFor(main, "Lapis.Library.PlayFeatured"));

        // WatchChromeWindow is a separate transparent owned Win32 top-level;
        // don't mistake the video NativeControlHost for Avalonia controls.
        var chrome = session.WaitForWatch();
        Assert.NotEqual(main.Properties.NativeWindowHandle.ValueOrDefault,
            chrome.Properties.NativeWindowHandle.ValueOrDefault);
        Assert.NotNull(session.WaitFor(chrome, "Lapis.Watch.PlayPause"));
        Assert.NotNull(session.WaitFor(chrome, "Lapis.Watch.Timeline"));
        session.Snapshot("04-watch");

        // Real sidecar subtitle from an actual media file, not injected text.
        var subtitle = session.WaitFor(chrome, "Lapis.Watch.Subtitle",
            TimeSpan.FromSeconds(12));
        Assert.Contains("Lapis E2E subtitle", subtitle.Name,
            StringComparison.OrdinalIgnoreCase);
        session.Snapshot("05-subtitle");

        session.Click(session.WaitFor(chrome, "Lapis.Watch.Back"));
        session.WaitFor(main, "Lapis.Library.PlayFeatured",
            TimeSpan.FromSeconds(15));
        session.Snapshot("06-back-to-library");
        Assert.True(session.ProcessIsRunning);
    }
}

internal sealed class DesktopSession : IDisposable
{
    private readonly string _root;
    private readonly string _mediaPath;
    private readonly string _catalogPath;
    private readonly string _artifactRoot;
    private Process? _process;
    private UIApp? _app;
    private UIA3Automation? _automation;
    private int _closed;

    private DesktopSession(string root, string mediaPath)
    {
        _root = root;
        _mediaPath = mediaPath;
        _catalogPath = Path.Combine(root, "catalog.json");
        _artifactRoot = Environment.GetEnvironmentVariable("LAPIS_E2E_ARTIFACTS")
            ?? Path.Combine(root, "artifacts");
        Directory.CreateDirectory(_artifactRoot);
    }

    public static async Task<DesktopSession> CreateAsync()
    {
        var original = Environment.GetEnvironmentVariable("LAPIS_E2E_MEDIA");
        Assert.False(string.IsNullOrWhiteSpace(original),
            "Set LAPIS_E2E_MEDIA to a real local MP4.");
        Assert.True(File.Exists(original), $"Missing media fixture: {original}");
        var root = Path.Combine(Path.GetTempPath(), "lapis-e2e-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        var file = Path.Combine(root, "Lapis-E2E.mp4");
        File.Copy(original!, file);
        // Keep the cue covering this existing short soak fixture.
        await File.WriteAllTextAsync(Path.ChangeExtension(file, ".srt"),
            "1\n00:00:00,000 --> 00:00:30,000\nLapis E2E subtitle\n");
        var session = new DesktopSession(root, file);
        await new JsonMediaLibrary(session._catalogPath)
            .ImportAsync(new MediaLocator(MediaLocatorKind.File, file));
        return session;
    }

    public async Task SeedFinishedWatchAsync()
    {
        var item = Assert.Single(await new JsonMediaLibrary(_catalogPath).ListAsync());
        await new JsonWatchProgressStore(_catalogPath + ".watch.v2.json").SaveAsync(
            new WatchProgress(item.Id, 239, 240, DateTimeOffset.UtcNow));
    }

    public async Task SeedWatchBookmarkAsync()
    {
        var item = Assert.Single(await new JsonMediaLibrary(_catalogPath).ListAsync());
        await new JsonWatchProgressStore(_catalogPath + ".watch.v2.json").SaveAsync(
            new WatchProgress(item.Id, 70, 240, DateTimeOffset.UtcNow));
    }

    public async Task SeedFavoriteAsync()
    {
        var movie = Assert.Single(await new JsonMediaLibrary(_catalogPath).ListAsync());
        await new JsonFavoriteStore(_catalogPath + ".favorites.v1.json")
            .SetAsync(movie.Id, true);
    }

    public bool MediaFileExists => File.Exists(_mediaPath);

    public string MoveMovieOffline()
    {
        // Move *after* catalog import but *before* process launch. The app
        // must keep the catalog entry and render a safe repair action.
        var newPath = Path.Combine(_root, "same-video-moved.mp4");
        File.Move(_mediaPath, newPath);
        return newPath;
    }

    public string AddPosterFixture()
    {
        var path = Path.Combine(_root, "Lapis-E2E.poster.png");
        // A real, small 2x2 PNG. The UI must decode it, not fake HasPoster.
        File.WriteAllBytes(path, Convert.FromBase64String(
            "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91Jpz" +
            "AAAAFklEQVR4nGPUmnOJgYGBiYGBgYGBAQASogGcsjuU1QAAAABJRU5ErkJggg=="));
        return path;
    }

    public void WaitForCatalogTitle(string expectedTitle) =>
        _ = Eventually(() =>
        {
            try
            {
                var items = new JsonMediaLibrary(_catalogPath)
                    .ListAsync().GetAwaiter().GetResult();
                return items.Count == 1 && items[0].Title == expectedTitle
                    ? new object() : null;
            }
            catch (IOException) { return null; }
        }, TimeSpan.FromSeconds(15), "persisted movie title " + expectedTitle);

    public void WaitForCatalogCount(int count) =>
        _ = Eventually(() =>
        {
            try
            {
                var items = new JsonMediaLibrary(_catalogPath)
                    .ListAsync().GetAwaiter().GetResult();
                return items.Count == count ? new object() : null;
            }
            catch (IOException) { return null; }
        }, TimeSpan.FromSeconds(15), "catalog count " + count);

    public void WaitForWatchCount(int count) =>
        _ = Eventually(() =>
        {
            try
            {
                var entries = new JsonWatchProgressStore(_catalogPath + ".watch.v2.json")
                    .LoadHistoryAsync().GetAwaiter().GetResult();
                return entries.Count == count ? new object() : null;
            }
            catch (IOException) { return null; }
        }, TimeSpan.FromSeconds(15), "watch history count " + count);

    public void WaitForFavoriteCount(int count) =>
        _ = Eventually(() =>
        {
            try
            {
                var entries = new JsonFavoriteStore(_catalogPath + ".favorites.v1.json")
                    .LoadAsync().GetAwaiter().GetResult();
                return entries.Count == count ? new object() : null;
            }
            catch (IOException) { return null; }
        }, TimeSpan.FromSeconds(15), "favorites count " + count);

    public bool ProcessIsRunning => _process is { HasExited: false };

    public void Start()
    {
        Assert.True(OperatingSystem.IsWindows());
        var exe = Environment.GetEnvironmentVariable("LAPIS_E2E_EXE");
        Assert.True(File.Exists(exe), $"Missing actual published Lapis executable: {exe}");
        var start = new ProcessStartInfo(exe!)
        {
            WorkingDirectory = Path.GetDirectoryName(exe!)!,
            UseShellExecute = false
        };
        start.Environment["LAPIS_E2E_MODE"] = "1";
        start.Environment["LAPIS_E2E_CATALOG"] = _catalogPath;
        _process = Process.Start(start)
            ?? throw new InvalidOperationException("Lapis process did not launch.");
        _app = UIApp.Attach(_process.Id);
        _automation = new UIA3Automation();
    }

    public Window MainWindow()
    {
        Assert.NotNull(_app);
        Assert.NotNull(_automation);
        // Fresh Windows GitHub runners occasionally let UIA return a
        // transient COM timeout while Avalonia initializes its desktop.
        // Retry the real EXE's main HWND (not a fake UI element), with an
        // overall deadline, process-exit detection and screenshot artifacts.
        return Eventually(() =>
        {
            try
            {
                return _app!.GetMainWindow(_automation!, TimeSpan.FromSeconds(4));
            }
            catch (TimeoutException)
            {
                return null;
            }
        }, TimeSpan.FromSeconds(30), "Lapis main window");
    }

    public Window WaitForWatch()
    {
        Assert.NotNull(_automation);
        return Eventually(() =>
        {
            // An Avalonia owned transparent popup may not be a direct child
            // in the UI Automation desktop tree. Get the actual HWND using
            // Win32, then ask UIA for the element represented by that HWND.
            foreach (var hwnd in ProcessWindowHandles())
            {
                if (ReadWindowTitle(hwnd) != "Lapis Watch Controls")
                    continue;
                var element = _automation!.FromHandle(hwnd);
                if (element is not null)
                    return element.AsWindow();
            }
            return null;
        }, TimeSpan.FromSeconds(18), "owned WatchChromeWindow");
    }

    private IReadOnlyList<nint> ProcessWindowHandles()
    {
        var list = new List<nint>();
        if (_process is null) return list;
        EnumWindows((hwnd, data) =>
        {
            GetWindowThreadProcessId(hwnd, out var processId);
            if (processId == (uint)_process.Id) list.Add(hwnd);
            return true;
        }, nint.Zero);
        return list;
    }

    private static string ReadWindowTitle(nint hwnd)
    {
        var text = new StringBuilder(256);
        _ = GetWindowText(hwnd, text, text.Capacity);
        return text.ToString();
    }

    private delegate bool EnumWindowsCallback(nint hwnd, nint data);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowsCallback callback, nint data);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(nint hwnd, StringBuilder title, int capacity);

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorRect
    {
        public int Left, Top, Right, Bottom;
    }

    private delegate bool MonitorEnumCallback(
        nint monitor, nint hdc, ref MonitorRect rect, nint data);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplayMonitors(
        nint hdc, nint clipRect, MonitorEnumCallback callback, nint data);

    public AutomationElement WaitFor(Window window, string automationId,
        TimeSpan? timeout = null) =>
        Eventually(() => window.FindFirstDescendant(
                cf => cf.ByAutomationId(automationId)),
            timeout ?? TimeSpan.FromSeconds(15), automationId);

    public AutomationElement WaitForEnabled(Window window, string automationId) =>
        Eventually(() =>
        {
            var element = window.FindFirstDescendant(
                cf => cf.ByAutomationId(automationId));
            return element?.IsEnabled == true ? element : null;
        }, TimeSpan.FromSeconds(15), automationId + " enabled");

    public void WaitForName(Window window, string automationId, string expected) =>
        _ = Eventually(() =>
        {
            var element = window.FindFirstDescendant(
                cf => cf.ByAutomationId(automationId));
            return element?.Name.Contains(expected, StringComparison.Ordinal) == true
                ? element : null;
        }, TimeSpan.FromSeconds(15), automationId + " " + expected);

    public void Click(AutomationElement element)
    {
        element.AsButton().Invoke();
    }

    private T Eventually<T>(Func<T?> find, TimeSpan timeout, string label)
        where T : class
    {
        var deadline = Stopwatch.StartNew();
        Exception? last = null;
        while (deadline.Elapsed < timeout)
        {
            if (_process?.HasExited == true)
                throw new InvalidOperationException(
                    $"Lapis exited unexpectedly with code {_process.ExitCode}, waiting for {label}");
            try
            {
                if (find() is { } found) return found;
            }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException
                or InvalidOperationException)
            {
                last = ex;
            }
            Thread.Sleep(120);
        }
        Snapshot("timeout-" + label.Replace('.', '-'));
        DumpAutomationTree();
        throw new TimeoutException(
            $"Timed out after {timeout.TotalSeconds:F0}s waiting for {label}. " +
            $"Last UIA error: {last?.Message}. Artifacts: {_artifactRoot}");
    }

    private static IReadOnlyList<Rectangle> EnumerateScreenBounds()
    {
        var monitors = new List<Rectangle>();
        MonitorEnumCallback callback = (nint monitor, nint hdc, ref MonitorRect rect, nint data) =>
        {
            monitors.Add(Rectangle.FromLTRB(rect.Left, rect.Top, rect.Right, rect.Bottom));
            return true;
        };
        if (!EnumDisplayMonitors(nint.Zero, nint.Zero, callback, nint.Zero))
            throw new InvalidOperationException(
                $"EnumDisplayMonitors failed: Win32 error {Marshal.GetLastWin32Error()}");
        return monitors;
    }

    public void Snapshot(string stage)
    {
        try
        {
            // Capture each physical desktop monitor; no Windows Forms dependency.
            var screens = EnumerateScreenBounds();
            for (var index = 0; index < screens.Count; index++)
            {
                var bounds = screens[index];
                if (bounds.Width <= 0 || bounds.Height <= 0) continue;
                using var bitmap = new Bitmap(bounds.Width, bounds.Height);
                using (var graphics = Graphics.FromImage(bitmap))
                    graphics.CopyFromScreen(bounds.Location, Point.Empty, bounds.Size);
                bitmap.Save(Path.Combine(_artifactRoot,
                    $"{stage}-monitor-{index}.png"), ImageFormat.Png);
            }
        }
        catch (Exception ex)
        {
            File.AppendAllText(Path.Combine(_artifactRoot, "diagnostics.txt"),
                $"Screenshot failed at {stage}: {ex}\n");
        }
    }

    private void DumpAutomationTree()
    {
        if (_automation is null) return;
        try
        {
            var sb = new StringBuilder();
            foreach (var window in _automation.GetDesktop().FindAllChildren(
                cf => cf.ByProcessId(_process!.Id)))
            {
                sb.AppendLine($"WINDOW {SafeName(window)} / {SafeId(window)}");
                foreach (var item in window.FindAllDescendants().Take(400))
                    sb.AppendLine($"  {item.ControlType}: {SafeId(item)} / {SafeName(item)}");
            }
            File.WriteAllText(Path.Combine(_artifactRoot, "uia-tree.txt"), sb.ToString());
        }
        catch (Exception ex)
        {
            File.AppendAllText(Path.Combine(_artifactRoot, "diagnostics.txt"),
                $"UIA tree capture failed: {ex}\n");
        }
    }

    private static string SafeId(AutomationElement item)
    {
        try { return item.AutomationId; }
        catch (FlaUI.Core.Exceptions.PropertyNotSupportedException) { return "(unsupported)"; }
    }

    private static string SafeName(AutomationElement item)
    {
        try { return item.Name; }
        catch (FlaUI.Core.Exceptions.PropertyNotSupportedException) { return "(unsupported)"; }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0) return;
        Snapshot("final");
        DumpAutomationTree();
        try
        {
            if (_process is { HasExited: false })
            {
                _process.Kill(entireProcessTree: true);
                _process.WaitForExit(5000);
            }
        }
        finally
        {
            _automation?.Dispose();
            _app?.Dispose();
            _process?.Dispose();
            // Test media/catalog stay in the CI temporary directory during
            // debugging, not the user's AppData. Remove after diagnostics.
            try { Directory.Delete(_root, recursive: true); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}
