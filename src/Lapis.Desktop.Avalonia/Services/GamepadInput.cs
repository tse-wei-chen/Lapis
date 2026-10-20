using System.Runtime.InteropServices;

namespace Lapis.Desktop.Avalonia.Services;

[Flags]
public enum GamepadButtons : ushort
{
    None = 0,
    Up = 0x0001,
    Down = 0x0002,
    Left = 0x0004,
    Right = 0x0008,
    Start = 0x0010,
    LeftShoulder = 0x0100,
    RightShoulder = 0x0200,
    A = 0x1000,
    B = 0x2000
}

public enum GamepadAction
{
    Up, Down, Left, Right, Accept, Back, PreviousCategory, NextCategory, Menu
}

public readonly record struct GamepadReading(
    GamepadButtons Buttons, short LeftX = 0, short LeftY = 0);

public interface IGamepadInputSource
{
    GamepadReading? Read();
}

/// <summary>
/// Windows inbox XInput 1.4 adapter. No device instance leaks into the
/// Avalonia layer, and disconnected controllers never generate commands.
/// Controller index is retained until disconnected (0-3 user slots).
/// </summary>
public sealed class WindowsXInputSource : IGamepadInputSource
{
    private int? _activeIndex;
    private bool _unavailable;

    public GamepadReading? Read()
    {
        if (_unavailable || !OperatingSystem.IsWindows()) return null;
        try
        {
            if (_activeIndex is { } index)
            {
                if (TryRead(index, out var connected)) return Decode(connected);
                // Deliver a disconnected sample first; this resets held
                // buttons before adopting a different controller on next tick.
                _activeIndex = null;
                return null;
            }

            for (var i = 0; i < 4; i++)
            {
                if (!TryRead(i, out var state)) continue;
                _activeIndex = i;
                return Decode(state);
            }
            return null;
        }
        catch (DllNotFoundException) { _unavailable = true; return null; }
        catch (EntryPointNotFoundException) { _unavailable = true; return null; }
        catch (BadImageFormatException) { _unavailable = true; return null; }
    }

    private static bool TryRead(int index, out XInputState state) =>
        XInputGetState((uint)index, out state) == 0;

    private static GamepadReading Decode(XInputState state) =>
        new((GamepadButtons)state.Gamepad.Buttons,
            state.Gamepad.ThumbLX, state.Gamepad.ThumbLY);

    [StructLayout(LayoutKind.Sequential)]
    private struct XInputGamepad
    {
        public ushort Buttons;
        public byte LeftTrigger;
        public byte RightTrigger;
        public short ThumbLX;
        public short ThumbLY;
        public short ThumbRX;
        public short ThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct XInputState
    {
        public uint PacketNumber;
        public XInputGamepad Gamepad;
    }

    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState", ExactSpelling = true)]
    private static extern uint XInputGetState(uint index, out XInputState state);
}

/// <summary>
/// Deterministic, frame-independent navigation actions. Button actions fire
/// once on key-down; directional actions repeat after an initial delay.
/// Safe to feed with fake snapshots and a monotonic time in tests.
/// </summary>
public sealed class GamepadInputRepeater
{
    private static readonly TimeSpan InitialRepeatDelay = TimeSpan.FromMilliseconds(330);
    private static readonly TimeSpan RepeatInterval = TimeSpan.FromMilliseconds(110);
    private GamepadButtons _previousButtons;
    private GamepadAction? _heldDirection;
    private TimeSpan _nextRepeatAt;

    public void Reset()
    {
        _previousButtons = GamepadButtons.None;
        _heldDirection = null;
        _nextRepeatAt = TimeSpan.Zero;
    }

    public IReadOnlyList<GamepadAction> Update(GamepadReading? input, TimeSpan now)
    {
        if (input is null)
        {
            Reset();
            return [];
        }

        var buttons = input.Value.Buttons;
        var pressed = buttons & ~_previousButtons;
        _previousButtons = buttons;
        var actions = new List<GamepadAction>(3);

        if (pressed.HasFlag(GamepadButtons.B)) actions.Add(GamepadAction.Back);
        if (pressed.HasFlag(GamepadButtons.A)) actions.Add(GamepadAction.Accept);
        if (pressed.HasFlag(GamepadButtons.Start)) actions.Add(GamepadAction.Menu);
        if (pressed.HasFlag(GamepadButtons.LeftShoulder))
            actions.Add(GamepadAction.PreviousCategory);
        if (pressed.HasFlag(GamepadButtons.RightShoulder))
            actions.Add(GamepadAction.NextCategory);

        var direction = Direction(input.Value);
        if (direction != _heldDirection)
        {
            _heldDirection = direction;
            _nextRepeatAt = now + InitialRepeatDelay;
            if (direction is { } action) actions.Add(action);
        }
        else if (direction is { } repeating && now >= _nextRepeatAt)
        {
            _nextRepeatAt = now + RepeatInterval;
            actions.Add(repeating);
        }

        return actions;
    }

    public static GamepadAction? Direction(GamepadReading input)
    {
        var buttons = input.Buttons;
        // Physical D-pad takes priority over a drifting analog stick.
        if ((buttons & GamepadButtons.Up) != 0) return GamepadAction.Up;
        if ((buttons & GamepadButtons.Down) != 0) return GamepadAction.Down;
        if ((buttons & GamepadButtons.Left) != 0) return GamepadAction.Left;
        if ((buttons & GamepadButtons.Right) != 0) return GamepadAction.Right;

        const int deadzone = 18_000; // ~55% of full travel; no idle drift.
        var x = (int)input.LeftX;
        var y = (int)input.LeftY;
        if (Math.Max(Math.Abs(x), Math.Abs(y)) < deadzone) return null;
        return Math.Abs(x) >= Math.Abs(y)
            ? x > 0 ? GamepadAction.Right : GamepadAction.Left
            : y > 0 ? GamepadAction.Up : GamepadAction.Down;
    }
}
