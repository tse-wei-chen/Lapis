using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class LibraryMotionTests
{
    [Theory]
    [InlineData(24, 42, 27, 31.5)]
    [InlineData(128, 42, 27, 135.5)]
    [InlineData(0, 40, 28, 6)]
    public void SelectionIndicatorIsCenteredRegardlessOfLabelLength(
        double top, double rowHeight, double indicatorHeight, double expected)
    {
        Assert.Equal(expected, LibraryMotion.IndicatorTop(top, rowHeight, indicatorHeight));
    }

    [Theory]
    [InlineData(true, false, true)]
    [InlineData(true, true, false)]
    [InlineData(false, false, false)]
    [InlineData(false, true, false)]
    public void ReducedMotionOrSystemPreferenceAlwaysWins(
        bool systemAnimationsEnabled, bool reduceMotion, bool expected)
    {
        Assert.Equal(expected,
            LibraryMotion.ShouldAnimate(systemAnimationsEnabled, reduceMotion));
    }
}
