using System.Windows.Forms;

namespace Lapis.Desktop.WinForms;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        System.Windows.Forms.Application.Run(new PlayerForm());
    }
}
