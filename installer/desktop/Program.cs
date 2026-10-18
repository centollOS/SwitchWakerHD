using System;
using System.Diagnostics;
using Avalonia;

namespace SwitchWakerHD.Installer;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        Str.Override(MacStrings.Table);
        Str.Es = SystemLanguage().StartsWith("es", StringComparison.OrdinalIgnoreCase);
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    // An app opened from the Finder gets no LANG: the first of the Mac's preferred languages (System Settings >
    // General > Language & Region), as `defaults read -g AppleLanguages` lists them ("(\n    "es-ES",\n ...").
    static string SystemLanguage()
    {
        var lang = Environment.GetEnvironmentVariable("LANG") ?? "";
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
