using System.Security.Principal;
using System.Windows;

namespace SwitchWakerHD.Installer;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // app.manifest asks for elevation; this only trips if that was bypassed.
        if (!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))
        {
            MessageBox.Show(Str.Get("err_not_admin"), Str.Get("title"), MessageBoxButton.OK, MessageBoxImage.Warning);
            Shutdown(1);
            return;
        }
        base.OnStartup(e);
    }
}
