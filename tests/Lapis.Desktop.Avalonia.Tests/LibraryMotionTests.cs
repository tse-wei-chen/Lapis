using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class LibraryMotionTests
{
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
