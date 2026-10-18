using System;
using System.Diagnostics;
using System.Linq;
using Avalonia;

namespace SwitchWakerHD.Installer;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        Str.Override(DesktopStrings.For(OperatingSystem.IsMacOS()));
        Str.Es = SystemLanguage().StartsWith("es", StringComparison.OrdinalIgnoreCase);
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    // Linux: the locale variables. macOS: an app opened from the Finder gets no LANG, so the first of the Mac's
    // preferred languages (System Settings > General > Language & Region), as `defaults read -g AppleLanguages`
    // lists them ("(\n    "es-ES",\n ...").
    static string SystemLanguage()
    {
        var lang = new[] { "LC_ALL", "LC_MESSAGES", "LANG" }.Select(Environment.GetEnvironmentVariable)
                       .FirstOrDefault(v => !string.IsNullOrEmpty(v)) ?? "";
        if (!OperatingSystem.IsMacOS()) return lang;
        try
        {
            var psi = new ProcessStartInfo("/usr/bin/defaults", "read -g AppleLanguages")
                { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            using var p = Process.Start(psi)!;
            var output = p.StandardOutput.ReadToEnd();
            p.WaitForExit(3000);
            var start = output.IndexOf('"');
            if (start >= 0) lang = output.Substring(start + 1);
        }
        catch (Exception) { }
        return lang;
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace();
}
