using Lapis.Contracts.Media;
using Lapis.Desktop.WinForms;
using Xunit;

namespace Lapis.Desktop.WinForms.Tests;

public sealed class BdmvDirectorySelectionTests
{
    [Fact]
    public void AcceptsDiscRootAndBdmvSubdirectoryWithoutChangingLocatorKind()
    {
        var root = Path.Combine(Path.GetTempPath(), "lapis-bdmv-ui-" + Guid.NewGuid());
        var bdmv = Path.Combine(root, "BDMV");
        try
        {
            Directory.CreateDirectory(Path.Combine(bdmv, "PLAYLIST"));
            Directory.CreateDirectory(Path.Combine(bdmv, "STREAM"));
            File.WriteAllText(Path.Combine(bdmv, "index.bdmv"), "fixture");

            Assert.True(BdmvDirectorySelection.TryCreate(root, out var disc));
            Assert.Equal(MediaLocatorKind.Directory, disc!.Kind);
            Assert.Equal(Path.GetFullPath(root), disc.Value);
            Assert.True(BdmvDirectorySelection.TryCreate(bdmv, out var inner));
            Assert.Equal(MediaLocatorKind.Directory, inner!.Kind);
            Assert.Equal(Path.GetFullPath(bdmv), inner.Value);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [Fact]
    public void RejectsUnsupportedDirectoriesAndEmptyPath()
    {
        var root = Path.Combine(Path.GetTempPath(), "lapis-bdmv-invalid-" + Guid.NewGuid());
        try
        {
            Directory.CreateDirectory(root);
            Assert.False(BdmvDirectorySelection.TryCreate(root, out var empty));
            Assert.Null(empty);
            Assert.False(BdmvDirectorySelection.TryCreate(null, out _));
            Assert.False(BdmvDirectorySelection.TryCreate(" ", out _));
            var bdmv = Path.Combine(root, "BDMV");
            Directory.CreateDirectory(Path.Combine(bdmv, "PLAYLIST"));
            Directory.CreateDirectory(Path.Combine(bdmv, "STREAM"));
            Assert.False(BdmvDirectorySelection.TryCreate(root, out _));
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }
}
