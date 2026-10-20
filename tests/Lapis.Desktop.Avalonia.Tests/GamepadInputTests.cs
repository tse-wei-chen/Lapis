using Lapis.Desktop.Avalonia.Services;
using Xunit;

namespace Lapis.Desktop.Avalonia.Tests;

public sealed class GamepadInputTests
{
    private static TimeSpan Ms(int milliseconds) => TimeSpan.FromMilliseconds(milliseconds);

    [Fact]
    public void AnalogDeadzoneAndDominantAxisSuppressDrift()
    {
        Assert.Null(GamepadInputRepeater.Direction(new GamepadReading(
            GamepadButtons.None, 9000, -12000)));
        Assert.Equal(GamepadAction.Right, GamepadInputRepeater.Direction(
            new GamepadReading(GamepadButtons.None, 23000, 8000)));
        Assert.Equal(GamepadAction.Up, GamepadInputRepeater.Direction(
            new GamepadReading(GamepadButtons.None, 11000, 27000)));
        Assert.Equal(GamepadAction.Down, GamepadInputRepeater.Direction(
            new GamepadReading(GamepadButtons.None, 500, -26000)));
        Assert.Equal(GamepadAction.Left, GamepadInputRepeater.Direction(
            new GamepadReading(GamepadButtons.Left, 28000, 12000)));
    }

    [Fact]
    public void DirectionRepeatsOnlyAfterInitialDelayAndStopsAfterRelease()
    {
        var repeat = new GamepadInputRepeater();
        var held = new GamepadReading(GamepadButtons.Right);
        Assert.Equal([GamepadAction.Right], repeat.Update(held, Ms(0)));
        Assert.Empty(repeat.Update(held, Ms(100)));
        Assert.Empty(repeat.Update(held, Ms(329)));
        Assert.Equal([GamepadAction.Right], repeat.Update(held, Ms(330)));
        Assert.Empty(repeat.Update(held, Ms(390)));
        Assert.Equal([GamepadAction.Right], repeat.Update(held, Ms(440)));
        Assert.Empty(repeat.Update(new GamepadReading(GamepadButtons.None), Ms(460)));
        Assert.Equal([GamepadAction.Right], repeat.Update(held, Ms(470)));
    }

    [Fact]
    public void ChangingDirectionDoesNotKeepOldRepeatDeadline()
    {
        var repeat = new GamepadInputRepeater();
        Assert.Equal([GamepadAction.Left], repeat.Update(
            new GamepadReading(GamepadButtons.Left), Ms(0)));
        Assert.Equal([GamepadAction.Up], repeat.Update(
            new GamepadReading(GamepadButtons.Up), Ms(120)));
        Assert.Empty(repeat.Update(new GamepadReading(GamepadButtons.Up), Ms(330)));
        Assert.Equal([GamepadAction.Up], repeat.Update(
            new GamepadReading(GamepadButtons.Up), Ms(450)));
    }

    [Fact]
    public void AcceptBackMenuAndBumpersAreEdgesNotRepeats()
    {
        var repeat = new GamepadInputRepeater();
        var pressed = new GamepadReading(GamepadButtons.A | GamepadButtons.B |
            GamepadButtons.Start | GamepadButtons.LeftShoulder |
            GamepadButtons.RightShoulder);
        Assert.Equal([
            GamepadAction.Back, GamepadAction.Accept, GamepadAction.Menu,
            GamepadAction.PreviousCategory, GamepadAction.NextCategory
        ], repeat.Update(pressed, Ms(0)));
        Assert.Empty(repeat.Update(pressed, Ms(1500)));
        Assert.Empty(repeat.Update(new GamepadReading(GamepadButtons.None), Ms(1600)));
        Assert.Equal([GamepadAction.Accept], repeat.Update(
            new GamepadReading(GamepadButtons.A), Ms(1700)));
    }

    [Fact]
    public void DisconnectionClearsHeldStateAndNeverInventsAnAction()
    {
        var repeat = new GamepadInputRepeater();
        var held = new GamepadReading(GamepadButtons.Down | GamepadButtons.A);
        Assert.Equal([GamepadAction.Accept, GamepadAction.Down],
            repeat.Update(held, Ms(0)));
        Assert.Empty(repeat.Update(null, Ms(300)));
        Assert.Empty(repeat.Update(null, Ms(1000)));
        Assert.Equal([GamepadAction.Accept, GamepadAction.Down],
            repeat.Update(held, Ms(1200)));
    }

    [Fact]
    public void DpadBeatsStickAndAnalogDiagonalHasOneAction()
    {
        var repeat = new GamepadInputRepeater();
        var mixed = new GamepadReading(GamepadButtons.Up, 30000, -30000);
        Assert.Equal([GamepadAction.Up], repeat.Update(mixed, Ms(0)));
        var diagonal = new GamepadReading(GamepadButtons.None, 25000, 24000);
        Assert.Equal([GamepadAction.Right], repeat.Update(diagonal, Ms(30)));
    }
}
