using System;
using System.Linq;
using Avalonia;

namespace SwitchWakerHD.Installer;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        // Invariant globalization (no libicu): the language comes from the locale variables.
        var lang = new[] { "LC_ALL", "LC_MESSAGES", "LANG" }
            .Select(Environment.GetEnvironmentVariable).FirstOrDefault(v => !string.IsNullOrEmpty(v)) ?? "";
        Str.Es = lang.StartsWith("es", StringComparison.OrdinalIgnoreCase);
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace();
}
