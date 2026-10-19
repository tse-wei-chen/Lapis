using Lapis.Contracts.Media;
using Lapis.Library;
using Xunit;

namespace Lapis.Library.Tests;

public sealed class DiscArtworkLocatorTests
{
    [Fact]
    public void ChoosesLargeDiscMetadataJpegAndNeverBackupOrMakeMkv()
    {
        using var disc = new TempDisc();
        var meta = Path.Combine(disc.Root, "BDMV", "META", "DL");
        Directory.CreateDirectory(meta);
        var backup = Path.Combine(disc.Root, "BDMV", "BACKUP", "META", "DL");
        Directory.CreateDirectory(backup);
        var other = Path.Combine(disc.Root, "MAKEMKV", "AACS");
        Directory.CreateDirectory(other);

        File.WriteAllText(Path.Combine(meta, "RSL_wakareD1_S.jpg"), "small");
        File.WriteAllText(Path.Combine(meta, "RSL_wakareD1_L.jpg"), "large");
        File.WriteAllText(Path.Combine(meta, "unrelated.png"), "png");
        File.WriteAllText(Path.Combine(backup, "back_L.jpg"), "backup");
        File.WriteAllText(Path.Combine(other, "other_L.jpg"), "keys");

        Assert.Equal(Path.Combine(meta, "RSL_wakareD1_L.jpg"),
            DiscArtworkLocator.FindPreferredArtwork(disc.Item));
    }

    [Fact]
    public void SkipsOversizedEmptyAndNonJpegAndReturnsNullWithoutMetadata()
    {
        using var disc = new TempDisc();
        Assert.Null(DiscArtworkLocator.FindPreferredArtwork(disc.Item));
        var meta = Path.Combine(disc.Root, "BDMV", "META", "DL");
        Directory.CreateDirectory(meta);
        File.WriteAllText(Path.Combine(meta, "empty_L.jpg"), "");
        File.WriteAllText(Path.Combine(meta, "fake_L.png"), "image");
        using (var huge = File.Create(Path.Combine(meta, "huge_L.jpg")))
            huge.SetLength(DiscArtworkLocator.MaxSourceBytes + 1);
        Assert.Null(DiscArtworkLocator.FindPreferredArtwork(disc.Item));

        File.WriteAllText(Path.Combine(meta, "small_S.JPEG"), "preview");
        Assert.Equal(Path.Combine(meta, "small_S.JPEG"),
            DiscArtworkLocator.FindPreferredArtwork(disc.Item));
    }

    [Fact]
    public void NeverTriesToFindDiscArtForOrdinaryVideoOrMissingDisc()
    {
        using var disc = new TempDisc();
        Assert.Null(DiscArtworkLocator.FindPreferredArtwork(disc.Item with
        {
            Kind = MediaLocatorKind.File
        }));
        Directory.Delete(disc.Root, recursive: true);
        Assert.Null(DiscArtworkLocator.FindPreferredArtwork(disc.Item));
    }

    private sealed class TempDisc : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(),
            "lapis-disc-art-" + Guid.NewGuid().ToString("N"));
        public MediaLibraryItem Item { get; }
        public TempDisc()
        {
            Directory.CreateDirectory(Root);
            Item = new MediaLibraryItem(Guid.NewGuid(), MediaLocatorKind.Directory,
                Root, "Wakare", DateTimeOffset.UtcNow);
        }
        public void Dispose()
        {
            if (Directory.Exists(Root)) Directory.Delete(Root, recursive: true);
        }
    }
}
