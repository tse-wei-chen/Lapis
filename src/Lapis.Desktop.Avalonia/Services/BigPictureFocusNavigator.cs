using Avalonia;

namespace Lapis.Desktop.Avalonia.Services;

public enum BigPictureDirection { Left, Right, Up, Down }

/// <summary>
/// Geometry-based focus direction, independent of ItemsControl, item sizes,
/// renderer, and current mouse input. A two-dimensional shelf and poster grid
/// use the same focus policy (keyboard and future controller adapters).
/// </summary>
public static class BigPictureFocusNavigator
{
    public static int FindNext(
        IReadOnlyList<Point> centers, int currentIndex, BigPictureDirection direction)
    {
        ArgumentNullException.ThrowIfNull(centers);
        if (centers.Count == 0) return -1;
        if (currentIndex < 0 || currentIndex >= centers.Count)
            return FindFirstFinite(centers);
        var origin = centers[currentIndex];
        if (!double.IsFinite(origin.X) || !double.IsFinite(origin.Y))
            return FindFirstFinite(centers);

        var best = -1;
        var bestScore = double.PositiveInfinity;
        for (var i = 0; i < centers.Count; i++)
        {
            if (i == currentIndex) continue;
            var item = centers[i];
            if (!double.IsFinite(item.X) || !double.IsFinite(item.Y)) continue;
            var dx = item.X - origin.X;
            var dy = item.Y - origin.Y;
            var primary = direction switch
            {
                BigPictureDirection.Left => -dx,
                BigPictureDirection.Right => dx,
                BigPictureDirection.Up => -dy,
                _ => dy
            };
            if (primary <= 3) continue;
            var secondary = direction is BigPictureDirection.Left or BigPictureDirection.Right
                ? Math.Abs(dy) : Math.Abs(dx);
            // Prefer staying within a shelf/column; without this a far
            // diagonal tile could win over the obvious adjacent neighbor.
            var score = primary + secondary * 2.5;
            if (score < bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }

    private static int FindFirstFinite(IReadOnlyList<Point> centers)
    {
        for (var i = 0; i < centers.Count; i++)
            if (double.IsFinite(centers[i].X) && double.IsFinite(centers[i].Y))
                return i;
        return -1;
    }
}
