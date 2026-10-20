using Avalonia;
using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class BigPictureFocusNavigatorTests
{
    private static readonly Point[] Grid =
    [
        new(100, 100), new(400, 100), new(700, 100),
        new(100, 340), new(400, 340), new(700, 340)
    ];

    [Fact]
    public void NavigationFollowsRowsAndColumns()
    {
        Assert.Equal(1, BigPictureFocusNavigator.FindNext(Grid, 0, BigPictureDirection.Right));
        Assert.Equal(0, BigPictureFocusNavigator.FindNext(Grid, 1, BigPictureDirection.Left));
        Assert.Equal(4, BigPictureFocusNavigator.FindNext(Grid, 1, BigPictureDirection.Down));
        Assert.Equal(1, BigPictureFocusNavigator.FindNext(Grid, 4, BigPictureDirection.Up));
        Assert.Equal(-1, BigPictureFocusNavigator.FindNext(Grid, 0, BigPictureDirection.Left));
    }

    [Fact]
    public void MissingFocusSelectsFirstFiniteControl()
    {
        Assert.Equal(0, BigPictureFocusNavigator.FindNext(Grid, -1, BigPictureDirection.Down));
        Assert.Equal(-1, BigPictureFocusNavigator.FindNext([], 0, BigPictureDirection.Right));
        Assert.Equal(1, BigPictureFocusNavigator.FindNext(
            [new Point(double.NaN, 1), new Point(100, 100)], -1,
            BigPictureDirection.Right));
    }
}
