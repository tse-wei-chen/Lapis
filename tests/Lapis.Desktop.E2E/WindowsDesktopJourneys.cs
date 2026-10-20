using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
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

        // UIA Invoke activates the real product Button, not the ViewModel.
        session.Click(session.WaitFor(main, "Lapis.Library.Manage"));
        session.WaitFor(main, "Lapis.Library.ManagementBody");
        session.Snapshot("02-management");
        Assert.Null(main.FindFirstDescendant(cf => cf.ByAutomationId(
            "Lapis.Library.PlayFeatured")));

        main.Focus();
        Keyboard.Press(VirtualKeyShort.ESCAPE);
        Assert.NotNull(session.WaitFor(main, "Lapis.Library.PlayFeatured"));
        session.Snapshot("03-library-restored");
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
        return _app!.GetMainWindow(_automation!, TimeSpan.FromSeconds(20))
            ?? throw new InvalidOperationException("No real Lapis main window.");
    }

    public Window WaitForWatch()
    {
        Assert.NotNull(_automation);
        return Eventually(() =>
        {
            var windows = _automation!.GetDesktop().FindAllChildren(
                cf => cf.ByProcessId(_process!.Id));
            return windows.Select(x => x.AsWindow())
                .FirstOrDefault(w => w is not null &&
                    w.Title == "Lapis Watch Controls");
        }, TimeSpan.FromSeconds(18), "owned WatchChromeWindow");
    }

    public AutomationElement WaitFor(Window window, string automationId,
        TimeSpan? timeout = null) =>
        Eventually(() => window.FindFirstDescendant(
                cf => cf.ByAutomationId(automationId)),
            timeout ?? TimeSpan.FromSeconds(15), automationId);

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

    public void Snapshot(string stage)
    {
        try
        {
            // Capture the real desktop pixels, not a headless visual tree.
            foreach (var (screen, index) in System.Windows.Forms.Screen.AllScreens
                .Select((screen, index) => (screen, index)))
            {
                var bounds = screen.Bounds;
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
                sb.AppendLine($"WINDOW {window.Name} / {window.AutomationId}");
                foreach (var item in window.FindAllDescendants().Take(400))
                    sb.AppendLine($"  {item.ControlType}: {item.AutomationId} / {item.Name}");
            }
            File.WriteAllText(Path.Combine(_artifactRoot, "uia-tree.txt"), sb.ToString());
        }
        catch (Exception ex)
        {
            File.AppendAllText(Path.Combine(_artifactRoot, "diagnostics.txt"),
                $"UIA tree capture failed: {ex}\n");
        }
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
