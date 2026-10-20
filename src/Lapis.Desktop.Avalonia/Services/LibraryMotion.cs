using System.Runtime.InteropServices;
using global::Avalonia;
using global::Avalonia.Rendering.Composition;

namespace Lapis.Desktop.Avalonia.Services;

/// <summary>
/// Small, render-thread-only motion for the LIBRARY visual tree.
/// NativeControlHost / the D3D11 video HWND must never be composition-animated.
/// Keep motion short; never block input while an animation is running.
/// </summary>
public static class LibraryMotion
{
    private const uint SpiGetClientAreaAnimation = 0x1042;
    private const int EnterMilliseconds = 205;
    private const int IndicatorMilliseconds = 205;
    private const int HoverMilliseconds = 115;

    [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", SetLastError = true)]
    private static extern bool GetWindowsAnimationPreference(
        uint action, uint param, out int value, uint flags);

    /// <summary>
    /// Testing seam: user and Windows preferences both take precedence over
    /// visual polish. No composition objects need to exist to evaluate it.
    /// </summary>
    public static bool ShouldAnimate(bool systemAnimationsEnabled, bool reduceMotion) =>
        systemAnimationsEnabled && !reduceMotion;

    public static bool IsAnimationEnabled => CanAnimate();

    private static bool CanAnimate()
    {
        var overrideValue = Environment.GetEnvironmentVariable("LAPIS_REDUCE_MOTION");
        if (overrideValue == "1") return false;
        if (!OperatingSystem.IsWindows()) return true;

        return !GetWindowsAnimationPreference(
                   SpiGetClientAreaAnimation, 0, out var enabled, 0)
               || enabled != 0;
    }

    /// <summary>
    /// Subtle cross-view fade. Layout and scroll positions remain owned by
    /// Avalonia; no visual is recreated and no async delay is introduced.
    /// </summary>
    public static void Enter(Visual view)
    {
        if (!CanAnimate()) return;
        var visual = ElementComposition.GetElementVisual(view);
        if (visual is null) return;
        visual.StopAnimation("Opacity");
        var fade = visual.Compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(EnterMilliseconds);
        fade.InsertKeyFrame(0f, 0.72f);
        fade.InsertKeyFrame(1f, 1f, new global::Avalonia.Animation.Easings.CubicEaseOut());
        visual.StartAnimation("Opacity", fade);

        // Offset animation is deliberately restricted to an already-laid-out
        // library viewport; we animate around the *existing* position.
        // Never set native HWND bounds or animate a playback surface.
        var offset = visual.Offset;
        var move = visual.Compositor.CreateVector3DKeyFrameAnimation();
        move.Duration = TimeSpan.FromMilliseconds(EnterMilliseconds);
        move.InsertKeyFrame(0f, new Vector3D(offset.X, offset.Y + 8, offset.Z));
        move.InsertKeyFrame(1f, offset, new global::Avalonia.Animation.Easings.CubicEaseOut());
        visual.StartAnimation("Offset", move);
    }

    /// <summary>
    /// Center one consistent-height marker next to a navigation item.
    /// Geometry lives in the sidebar's layout coordinate space.
    /// </summary>
    public static double IndicatorTop(double itemTop, double itemHeight, double markerHeight) =>
        itemTop + (itemHeight - markerHeight) / 2.0;

    /// <summary>
    /// The sidebar has one marker, not one per navigation button. A
    /// Composition implicit animation tracks the latest Y destination, so
    /// rapid clicks never queue multiple stale navigation tweens.
    /// </summary>
    public static void MoveNavigationMarker(Visual marker, double y, bool animate)
    {
        var visual = ElementComposition.GetElementVisual(marker);
        if (visual is null) return;
        var target = new Vector3D(visual.Offset.X, (float)y, visual.Offset.Z);
        if (!animate || !CanAnimate())
        {
            visual.ImplicitAnimations = null;
            visual.StopAnimation("Offset");
            visual.Offset = target;
            return;
        }

        if (visual.ImplicitAnimations is null)
        {
            var slide = visual.Compositor.CreateVector3DKeyFrameAnimation();
            slide.Target = "Offset";
            slide.Duration = TimeSpan.FromMilliseconds(IndicatorMilliseconds);
            slide.InsertExpressionKeyFrame(1f, "this.FinalValue");
            var animations = visual.Compositor.CreateImplicitAnimationCollection();
            animations["Offset"] = slide;
            visual.ImplicitAnimations = animations;
        }

        visual.Offset = target;
    }

    /// <summary>
    /// Implicit animations interpolate to the most recent opacity target,
    /// including rapid pointer enter/exit. No UI-thread animations/timers.
    /// </summary>
    public static void Hover(Visual view, bool over)
    {
        var visual = ElementComposition.GetElementVisual(view);
        if (visual is null) return;

        if (!CanAnimate())
        {
            visual.Opacity = over ? 0.92f : 1f;
            return;
        }

        if (visual.ImplicitAnimations is null)
        {
            var fade = visual.Compositor.CreateScalarKeyFrameAnimation();
            fade.Target = "Opacity";
            fade.Duration = TimeSpan.FromMilliseconds(HoverMilliseconds);
            fade.InsertExpressionKeyFrame(1f, "this.FinalValue");
            var animations = visual.Compositor.CreateImplicitAnimationCollection();
            animations["Opacity"] = fade;
            visual.ImplicitAnimations = animations;
        }

        visual.Opacity = over ? 0.92f : 1f;
    }
}
