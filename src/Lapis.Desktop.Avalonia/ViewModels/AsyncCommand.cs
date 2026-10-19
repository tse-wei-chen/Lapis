using System.Windows.Input;

namespace Lapis.Desktop.Avalonia.ViewModels;

/// <summary>Minimal MVVM command; no framework or Win32 dependency.</summary>
public sealed class AsyncCommand(Func<Task> execute) : ICommand
{
    public bool CanExecute(object? parameter) => true;
    public event EventHandler? CanExecuteChanged
    {
        add { }
        remove { }
    }

    public async void Execute(object? parameter)
    {
        // PlayerViewModel handles and reports user-command errors, so an
        // async-void ICommand handler cannot leak unobserved UI exceptions.
        await execute();
    }
}
