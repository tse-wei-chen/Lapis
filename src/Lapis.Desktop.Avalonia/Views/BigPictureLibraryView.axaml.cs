using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
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
    private readonly Grid _detailsOverlay;
    private readonly Button _detailsPlay;
    private readonly Button _detailsResume;
    private readonly Button _detailsRepair;
    private readonly Button _renameButton;
    private readonly Button _removeButton;
    private readonly Button _removeCancel;
    private readonly StackPanel _removeConfirmation;
    private readonly StackPanel _renameRow;
    private readonly TextBox _renameInput;
    private readonly TextBlock _renameError;
    private Button? _focusedTile;

    public event Action<LibraryCardViewModel>? PlayRequested;
    public event Action<LibraryCardViewModel>? ResumeRequested;
    public event Action<LibraryCardViewModel>? ForgetHistoryRequested;
    public event Action<LibraryCardViewModel>? ToggleFavoriteRequested;
    public event Action<LibraryCardViewModel>? RepairSourceRequested;
    public event Action<LibraryCardViewModel, string>? RenameTitleRequested;
    public event Action<LibraryCardViewModel>? RemoveFromLibraryRequested;
    public event Action? ImportRequested;
    public event Action? ImportDiscRequested;

    public BigPictureLibraryView()
    {
        AvaloniaXamlLoader.Load(this);
        _scroll = this.FindControl<ScrollViewer>("BigPictureScroll")
            ?? throw new InvalidOperationException("Missing Big Picture scroll.");
        _hero = this.FindControl<Border>("BigPictureHero")
            ?? throw new InvalidOperationException("Missing Big Picture hero.");
        _detailsOverlay = this.FindControl<Grid>("BigPictureDetailsOverlay")
            ?? throw new InvalidOperationException("Missing Big Picture film details.");
        _detailsPlay = this.FindControl<Button>("BigPictureDetailsPlay")
            ?? throw new InvalidOperationException("Missing film detail play action.");
        _detailsResume = this.FindControl<Button>("BigPictureDetailsResume")
            ?? throw new InvalidOperationException("Missing film detail resume action.");
        _detailsRepair = this.FindControl<Button>("BigPictureDetailsRepair")
            ?? throw new InvalidOperationException("Missing source repair action.");
        _renameButton = this.FindControl<Button>("BigPictureDetailsRename")
            ?? throw new InvalidOperationException("Missing edit-title action.");
        _renameRow = this.FindControl<StackPanel>("BigPictureRenameEditor")
            ?? throw new InvalidOperationException("Missing edit-title row.");
        _renameInput = this.FindControl<TextBox>("BigPictureRenameInput")
            ?? throw new InvalidOperationException("Missing title editor.");
        _renameError = this.FindControl<TextBlock>("BigPictureRenameError")
            ?? throw new InvalidOperationException("Missing title validation label.");
        _removeButton = this.FindControl<Button>("BigPictureDetailsRemove")
            ?? throw new InvalidOperationException("Missing remove-from-library action.");
        _removeCancel = this.FindControl<Button>("BigPictureRemoveCancel")
            ?? throw new InvalidOperationException("Missing remove confirmation cancel.");
        _removeConfirmation = this.FindControl<StackPanel>("BigPictureRemoveConfirmation")
            ?? throw new InvalidOperationException("Missing remove confirmation.");
        // Tunnel before Button/ScrollViewer swallow navigation arrows.
        AddHandler(InputElement.KeyDownEvent, BigPictureKeyDown,
            RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    private LibraryViewModel? Library => DataContext as LibraryViewModel;

    /// <summary>
    /// Return keyboard/controller focus to the previously selected movie
    /// after exiting Watch, without recreating or re-sorting the catalog.
    /// </summary>
    public void ResetScroll() => _scroll.Offset = new Vector(0, 0);

    public void CloseDetails()
    {
        if (!_detailsOverlay.IsVisible) return;
        _detailsOverlay.IsVisible = false;
        CancelRenameEditor();
        _removeConfirmation.IsVisible = false;
        if (IsVisible) _focusedTile?.Focus();
    }

    private void OpenDetails(LibraryCardViewModel card, Button? tile)
    {
        if (tile is not null) _focusedTile = tile;
        Library?.SelectFeatured(card);
        CancelRenameEditor();
        _removeConfirmation.IsVisible = false;
        card.RefreshSourceAvailability();
        _detailsOverlay.IsVisible = true;
        if (card.NeedsSourceRepair) _detailsRepair.Focus();
        else if (card.CanResume) _detailsResume.Focus();
        else _detailsPlay.Focus();
        LibraryMotion.Enter(_detailsOverlay);
    }

    private void FeaturedDetails_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is { } card) OpenDetails(card, null);
    }

    private void DetailsClose_Click(object? sender, RoutedEventArgs e) =>
        CloseDetails();

    private void DetailsRemove_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is null) return;
        CancelRenameEditor();
        _removeConfirmation.IsVisible = true;
        _removeCancel.Focus(); // Gamepad/keyboard Back and Cancel are safe defaults.
    }

    private void DetailsRemoveCancel_Click(object? sender, RoutedEventArgs e)
    {
        _removeConfirmation.IsVisible = false;
        _removeButton.Focus();
    }

    private void DetailsRemoveConfirm_Click(object? sender, RoutedEventArgs e)
    {
        if (!_removeConfirmation.IsVisible || Library?.FeaturedCard is not { } card) return;
        CloseDetails();
        RemoveFromLibraryRequested?.Invoke(card);
    }

    private void DetailsRename_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is not { } card) return;
        _renameInput.Text = card.Title;
        _renameError.IsVisible = false;
        _renameRow.IsVisible = true;
        _renameButton.IsVisible = false;
        _renameInput.Focus();
        _renameInput.SelectAll();
    }

    private void DetailsRenameCancel_Click(object? sender, RoutedEventArgs e)
    {
        CancelRenameEditor();
        _detailsPlay.Focus();
    }

    private void CancelRenameEditor()
    {
        _renameRow.IsVisible = false;
        _renameButton.IsVisible = true;
        _renameError.IsVisible = false;
        _renameInput.Text = "";
    }

    private void DetailsRenameSave_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is not { } card) return;
        var title = (_renameInput.Text ?? "").Trim();
        if (title.Length is 0 or > 160 || title.Any(char.IsControl))
        {
            _renameError.Text = "片名需介於 1–160 字元，不能包含換行或控制字元。";
            _renameError.IsVisible = true;
            return;
        }

        CloseDetails();
        RenameTitleRequested?.Invoke(card, title);
    }

    private void DetailsPlay_Click(object? sender, RoutedEventArgs e)
    {
        if (!_detailsPlay.IsEnabled || Library?.FeaturedCard is not { } card) return;
        CloseDetails();
        PlayRequested?.Invoke(card);
    }

    private void DetailsResume_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is not { CanResume: true } card) return;
        CloseDetails();
        ResumeRequested?.Invoke(card);
    }

    private void DetailsRefreshPoster_Click(object? sender, RoutedEventArgs e)
    {
        // Keep the details overlay open while the existing card reloads.
        Library?.FeaturedCard?.RefreshPoster();
    }

    private void DetailsRepairSource_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is not { NeedsSourceRepair: true } card) return;
        CloseDetails();
        RepairSourceRequested?.Invoke(card);
    }

    private void DetailsFavorite_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is not { } card) return;
        // Removing the final pinned movie hides it from this category; leave
        // the detail sheet before the selected card disappears.
        if (Library.IsFavoritesCategory && card.IsFavorite) CloseDetails();
        ToggleFavoriteRequested?.Invoke(card);
    }

    private void DetailsForget_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is not { HasWatchHistory: true } card) return;
        CloseDetails();
        ForgetHistoryRequested?.Invoke(card);
    }

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
    private void History_Click(object? sender, RoutedEventArgs e) =>
        Navigate(() => Library?.ShowHistory());
    private void Favorites_Click(object? sender, RoutedEventArgs e) =>
        Navigate(() => Library?.ShowFavorites());

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
        if (sender is Button { CommandParameter: LibraryCardViewModel card } tile)
            OpenDetails(card, tile);
    }

    private void FeaturedPlay_Click(object? sender, RoutedEventArgs e)
    {
        if (Library?.FeaturedCard is not { } card) return;
        if (card.CanResume) ResumeRequested?.Invoke(card);
        else PlayRequested?.Invoke(card);
    }

    private void ContextPlay_Click(object? sender, RoutedEventArgs e)
    {
        if (sender is MenuItem { CommandParameter: LibraryCardViewModel card })
            PlayRequested?.Invoke(card);
    }

    private void ContextManage_Click(object? sender, RoutedEventArgs e)
    {
        if (sender is MenuItem { CommandParameter: LibraryCardViewModel card })
            OpenDetails(card, null);
    }

    private void Grid_Click(object? sender, RoutedEventArgs e) => SetDisplayMode(false);
    private void List_Click(object? sender, RoutedEventArgs e) => SetDisplayMode(true);

    private void SetDisplayMode(bool list)
    {
        if (Library is not { } view) return;
        CloseDetails();
        if (list) view.ShowList();
        else view.ShowGrid();
        _scroll.Offset = new Vector(0, 0);
        Dispatcher.UIThread.Post(RestoreMovieFocus, DispatcherPriority.Loaded);
    }

    private void Import_Click(object? sender, RoutedEventArgs e) =>
        ImportRequested?.Invoke();

    private void ImportDisc_Click(object? sender, RoutedEventArgs e) =>
        ImportDiscRequested?.Invoke();

    private static readonly LibraryCategory[] Categories =
    [
        LibraryCategory.All, LibraryCategory.RecentlyAdded,
        LibraryCategory.BluRay, LibraryCategory.Videos,
        LibraryCategory.History, LibraryCategory.Favorites
    ];

    /// <summary>
    /// One semantic input path shared by keyboard navigation, XInput and a
    /// future Steam Input / SDL adapter. The catalog and Playback Actor
    /// remain unaware of physical controller events.
    /// </summary>
    public void HandleGamepadAction(GamepadAction action)
    {
        if (!IsVisible) return;
        switch (action)
        {
            case GamepadAction.Up:
            case GamepadAction.Down:
            case GamepadAction.Left:
            case GamepadAction.Right:
                MoveFocus(action switch
                {
                    GamepadAction.Up => BigPictureDirection.Up,
                    GamepadAction.Down => BigPictureDirection.Down,
                    GamepadAction.Left => BigPictureDirection.Left,
                    _ => BigPictureDirection.Right
                });
                break;
            case GamepadAction.Accept:
                var focus = InteractiveButtons().FirstOrDefault(b => b.IsFocused);
                if (focus is null)
                {
                    if (_detailsOverlay.IsVisible)
                    {
                        var targetButton = _removeConfirmation.IsVisible
                            ? _removeCancel
                            : _detailsResume.IsVisible ? _detailsResume : _detailsPlay;
                        if (targetButton.IsEnabled)
                            targetButton.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                    }
                    else if (!_detailsOverlay.IsVisible && Library?.FeaturedCard is { } card)
                        OpenDetails(card, null);
                }
                else
                {
                    focus.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                }
                break;
            case GamepadAction.Back:
                if (_detailsOverlay.IsVisible)
                {
                    if (_removeConfirmation.IsVisible)
                        DetailsRemoveCancel_Click(null, new RoutedEventArgs());
                    else
                        CloseDetails();
                    break;
                }
                Navigate(() =>
                {
                    Library?.ShowAll();
                    if (Library is { } view) view.SearchText = "";
                });
                RestoreMovieFocus();
                break;
            case GamepadAction.Menu:
                SetDisplayMode(Library?.IsGridView == true);
                break;
            case GamepadAction.PreviousCategory:
                if (!_detailsOverlay.IsVisible) CycleCategory(-1);
                break;
            case GamepadAction.NextCategory:
                if (!_detailsOverlay.IsVisible) CycleCategory(1);
                break;
        }
    }

    private void CycleCategory(int delta)
    {
        if (Library is not { } view) return;
        var index = Array.IndexOf(Categories, view.Category);
        var target = Categories[(Math.Max(0, index) + delta + Categories.Length)
            % Categories.Length];
        Navigate(() =>
        {
            switch (target)
            {
                case LibraryCategory.All: view.ShowAll(); break;
                case LibraryCategory.RecentlyAdded: view.ShowRecentlyAdded(); break;
                case LibraryCategory.BluRay: view.ShowDiscs(); break;
                case LibraryCategory.Videos: view.ShowVideos(); break;
                case LibraryCategory.History: view.ShowHistory(); break;
                case LibraryCategory.Favorites: view.ShowFavorites(); break;
            }
        });
        RestoreMovieFocus();
    }

    private Button[] InteractiveButtons() =>
        (_detailsOverlay.IsVisible ? _detailsOverlay : (Visual)this).GetVisualDescendants()
        .OfType<Button>()
        .Where(button => button.IsVisible &&
            button.Bounds.Width > 0 && button.Bounds.Height > 0 &&
            (button.Classes.Contains("bp-tile") ||
             button.Classes.Contains("bp-nav") ||
             button.Classes.Contains("bp-primary") ||
             button.Classes.Contains("bp-secondary") ||
             button.Classes.Contains("bp-mode")))
        .ToArray();

    private void MoveFocus(BigPictureDirection direction)
    {
        var candidates = InteractiveButtons();
        if (candidates.Length == 0) return;
        var positions = candidates.Select(b =>
            global::Avalonia.VisualExtensions.TranslatePoint(
                b, new Point(b.Bounds.Width / 2, b.Bounds.Height / 2), this)
                ?? new Point(double.NaN, double.NaN)).ToArray();

        var focused = Array.FindIndex(candidates, b => b.IsFocused);
        if (focused < 0 && _focusedTile is { } last)
            focused = Array.IndexOf(candidates, last);
        if (focused < 0 && Library?.FeaturedCard is { } selected)
            focused = Array.FindIndex(candidates,
                b => b.Classes.Contains("bp-tile") &&
                     ReferenceEquals(b.CommandParameter, selected));
        var next = BigPictureFocusNavigator.FindNext(positions, focused, direction);
        if (next < 0) return;
        var target = candidates[next];
        target.Focus();
        // Native D3D11 HWND is not in this visual tree. Only UI moves.
        LibraryMotion.Enter(target);
    }

    private void BigPictureKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Handled || !IsVisible) return;
        // Never hijack typing, ComboBox selection or a nested text editor.
        if (e.Source is Visual visual &&
            (visual is TextBox or ComboBox ||
             visual.GetVisualAncestors().Any(x => x is TextBox or ComboBox)))
            return;

        if (e.Key == Key.Escape)
        {
            HandleGamepadAction(GamepadAction.Back);
            e.Handled = true;
            return;
        }

        var action = e.Key switch
        {
            Key.Left => GamepadAction.Left,
            Key.Right => GamepadAction.Right,
            Key.Up => GamepadAction.Up,
            Key.Down => GamepadAction.Down,
            _ => (GamepadAction?)null
        };
        if (action is not { } move) return;
        HandleGamepadAction(move);
        e.Handled = true;
    }
}
