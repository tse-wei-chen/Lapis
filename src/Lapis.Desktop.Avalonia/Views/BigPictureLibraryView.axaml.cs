using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.VisualTree;
using Lapis.Desktop.Avalonia.Services;
using Lapis.Desktop.Avalonia.ViewModels;

namespace Lapis.Desktop.Avalonia.Views;

/// <summary>
/// Controller-sized presentation only. Persistent catalog / import / playback
/// stay owned by LibraryViewModel and MainWindow respectively.
/// </summary>
public sealed partial class BigPictureLibraryView : UserControl
{
    private readonly ScrollViewer _scroll;
    private readonly Border _hero;
    private Button? _focusedTile;

    public event Action<LibraryCardViewModel>? PlayRequested;
    public event Action? ImportRequested;
    public event Action? ImportDiscRequested;
    public event Action? ManageRequested;

    public BigPictureLibraryView()
    {
        AvaloniaXamlLoader.Load(this);
        _scroll = this.FindControl<ScrollViewer>("BigPictureScroll")
            ?? throw new InvalidOperationException("Missing Big Picture scroll.");
        _hero = this.FindControl<Border>("BigPictureHero")
            ?? throw new InvalidOperationException("Missing Big Picture hero.");
    }

    private LibraryViewModel? Library => DataContext as LibraryViewModel;

    /// <summary>
    /// Return keyboard/controller focus to the previously selected movie
    /// after exiting Watch, without recreating or re-sorting the catalog.
    /// </summary>
    public void RestoreMovieFocus()
    {
        if (!IsVisible) return;
        var selected = Library?.FeaturedCard;
        var tiles = this.GetVisualDescendants().OfType<Button>()
            .Where(b => b.IsVisible && b.Classes.Contains("bp-tile")).ToArray();
        var target = tiles.FirstOrDefault(b => ReferenceEquals(b.CommandParameter, selected))
            ?? tiles.FirstOrDefault();
        target?.Focus();
    }

    private void All_Click(object? sender, RoutedEventArgs e) =>
        Navigate(() => Library?.ShowAll());
    private void Recent_Click(object? sender, RoutedEventArgs e) =>
        Navigate(() => Library?.ShowRecentlyAdded());
    private void BluRay_Click(object? sender, RoutedEventArgs e) =>
        Navigate(() => Library?.ShowDiscs());
    private void Videos_Click(object? sender, RoutedEventArgs e) =>
        Navigate(() => Library?.ShowVideos());

    private void Navigate(Action navigate)
    {
        navigate();
        _scroll.Offset = new Vector(0, 0);
        _focusedTile = null;
        LibraryMotion.Enter(_hero);
    }

    private void TilePointerEntered(object? sender, PointerEventArgs e)
    {
        if (sender is Button { CommandParameter: LibraryCardViewModel card } button)
            FocusArtwork(button, card);
    }

    private void TileGotFocus(object? sender, GotFocusEventArgs e)
    {
        if (sender is Button { CommandParameter: LibraryCardViewModel card } button)
            FocusArtwork(button, card);
    }

    private void FocusArtwork(Button button, LibraryCardViewModel card)
    {
        var current = Library?.FeaturedCard;
        _focusedTile = button;
        Library?.SelectFeatured(card);
        if (!ReferenceEquals(current, card))
            LibraryMotion.Enter(_hero);
    }

    private void TilePlay_Click(object? sender, RoutedEventArgs e)
    {
        if (sender is Button { CommandParameter: LibraryCardViewModel card })
            PlayRequested?.Invoke(card);
    }

    private void FeaturedPlay_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is { } card)
            PlayRequested?.Invoke(card);
    }

    private void ContextPlay_Click(object? sender, RoutedEventArgs e)
    {
        if (sender is MenuItem { CommandParameter: LibraryCardViewModel card })
            PlayRequested?.Invoke(card);
    }

    private void ContextManage_Click(object? sender, RoutedEventArgs e) =>
        ManageRequested?.Invoke();

    private void Import_Click(object? sender, RoutedEventArgs e) =>
        ImportRequested?.Invoke();

    private void ImportDisc_Click(object? sender, RoutedEventArgs e) =>
        ImportDiscRequested?.Invoke();

    private void Manage_Click(object? sender, RoutedEventArgs e) =>
        ManageRequested?.Invoke();

    private void BigPictureKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Source is TextBox or ComboBox) return;
        if (e.Key == Key.Escape)
        {
            Library?.ShowAll();
            _scroll.Offset = new Vector(0, 0);
            e.Handled = true;
            return;
        }

        var direction = e.Key switch
        {
            Key.Left => BigPictureDirection.Left,
            Key.Right => BigPictureDirection.Right,
            Key.Up => BigPictureDirection.Up,
            Key.Down => BigPictureDirection.Down,
            _ => (BigPictureDirection?)null
        };
        if (direction is null) return;

        var tiles = this.GetVisualDescendants()
            .OfType<Button>()
            .Where(button => button.IsVisible && button.Classes.Contains("bp-tile"))
            .ToArray();
        if (tiles.Length == 0) return;

        var points = tiles.Select(tile =>
            global::Avalonia.VisualExtensions.TranslatePoint(
                tile, new Point(tile.Bounds.Width / 2, tile.Bounds.Height / 2), this)
                ?? new Point(double.NaN, double.NaN)).ToArray();
        var current = Array.IndexOf(tiles, _focusedTile);
        var next = BigPictureFocusNavigator.FindNext(points, current,
            direction.Value);
        if (next < 0) return;
        tiles[next].Focus();
        e.Handled = true;
    }
}
