using Lapis.Contracts.Media;
using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class BdmvLocatorTests
{
    [Fact]
    public void DiscRootAndBdmvFolderKeepDirectoryLocator()
    {
        var root = Path.Combine(Path.GetTempPath(), "lapis-avalonia-" + Guid.NewGuid());
        var bdmv = Path.Combine(root, "BDMV");
        try
        {
            Directory.CreateDirectory(Path.Combine(bdmv, "STREAM"));
            Directory.CreateDirectory(Path.Combine(bdmv, "PLAYLIST"));
            File.WriteAllText(Path.Combine(bdmv, "index.bdmv"), "test");
            Assert.True(BdmvLocator.TryFromDirectory(root, out var disc));
            Assert.Equal(MediaLocatorKind.Directory, disc!.Kind);
            Assert.Equal(Path.GetFullPath(root), disc.Value);
            Assert.True(BdmvLocator.TryFromDirectory(bdmv, out var child));
            Assert.Equal(MediaLocatorKind.Directory, child!.Kind);
            Assert.Equal(Path.GetFullPath(root), child.Value);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void ArbitraryFolderOrInvalidPathIsNotPresentedAsBluray()
    {
        var path = Path.Combine(Path.GetTempPath(), "lapis-not-bdmv-" + Guid.NewGuid());
        try
        {
            Directory.CreateDirectory(path);
            Assert.False(BdmvLocator.TryFromDirectory(path, out var media));
            Assert.Null(media);
            Assert.False(BdmvLocator.TryFromDirectory(null, out _));
            Assert.False(BdmvLocator.TryFromDirectory(" ", out _));
            Assert.False(BdmvLocator.TryFromDirectory(
                Path.Combine(path, "absent"), out _));
        }
        finally
        {
            if (Directory.Exists(path)) Directory.Delete(path, recursive: true);
        }
    }
}
