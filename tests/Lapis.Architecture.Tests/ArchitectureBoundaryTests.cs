using System.Xml.Linq;
using Xunit;

namespace Lapis.Architecture.Tests;

public sealed class ArchitectureBoundaryTests
{
    private static readonly string Root = FindRepositoryRoot();

    [Fact]
    public void ContractsHaveNoProjectReferences()
    {
        var project = LoadProject("src/Lapis.Contracts/Lapis.Contracts.csproj");
        Assert.Empty(project.Descendants("ProjectReference"));
    }

    [Fact]
    public void ApplicationOnlyReferencesContracts()
    {
        var project = LoadProject("src/Lapis.Application/Lapis.Application.csproj");
        var references = project
            .Descendants("ProjectReference")
            .Select(element => element.Attribute("Include")?.Value.Replace('\\', '/'))
            .Where(value => value is not null)
            .ToArray();

        Assert.Equal(new[] { "../Lapis.Contracts/Lapis.Contracts.csproj" }, references);
    }

    [Fact]
    public void ContractsDoNotLeakThirdPartyMediaTypes()
    {
        var forbidden = new[]
        {
            "AVFrame",
            "AVPacket",
            "AVCodecContext",
            "AVFormatContext",
            "BLURAY_",
            "libbluray",
            "FFmpeg"
        };

        var files = Directory.EnumerateFiles(
            Path.Combine(Root, "src", "Lapis.Contracts"),
            "*.cs",
            SearchOption.AllDirectories);

        foreach (var file in files)
        {
            var source = File.ReadAllText(file);
            foreach (var token in forbidden)
            {
                Assert.DoesNotContain(token, source, StringComparison.OrdinalIgnoreCase);
            }
        }
    }

    [Fact]
    public void ApplicationDoesNotOwnNativeInterop()
    {
        var files = Directory.EnumerateFiles(
            Path.Combine(Root, "src", "Lapis.Application"),
            "*.cs",
            SearchOption.AllDirectories);

        foreach (var file in files)
        {
            var source = File.ReadAllText(file);
            Assert.DoesNotContain("DllImport", source, StringComparison.Ordinal);
            Assert.DoesNotContain("LibraryImport", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void NativeNonAdapterCodeDoesNotUseFfmpegTypes()
    {
        var nativeRoot = Path.Combine(Root, "native", "MediaRuntime", "src");
        var forbidden = new[]
        {
            "#include <libav",
            "AVFrame",
            "AVPacket",
            "AVCodecContext",
            "AVCodecParameters",
            "AVFormatContext",
            "AVRational",
            "AVPixelFormat"
        };

        var files = Directory.EnumerateFiles(nativeRoot, "*.*", SearchOption.AllDirectories)
            .Where(file =>
                file.EndsWith(".h", StringComparison.OrdinalIgnoreCase) ||
                file.EndsWith(".hpp", StringComparison.OrdinalIgnoreCase) ||
                file.EndsWith(".c", StringComparison.OrdinalIgnoreCase) ||
                file.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase));

        foreach (var file in files)
        {
            var relative = Path.GetRelativePath(nativeRoot, file).Replace('\\', '/');
            if ($"/{relative}/".Contains("/ffmpeg/", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var source = File.ReadAllText(file);
            foreach (var token in forbidden)
            {
                Assert.DoesNotContain(token, source, StringComparison.Ordinal);
            }
        }
    }

    private static XDocument LoadProject(string relativePath) =>
        XDocument.Load(Path.Combine(Root, relativePath.Replace('/', Path.DirectorySeparatorChar)));

    private static string FindRepositoryRoot()
    {
        DirectoryInfo? current = new(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "Directory.Build.props")))
            {
                return current.FullName;
            }

            current = current.Parent;
        }

        throw new InvalidOperationException("Could not locate the Lapis repository root.");
    }
}
