// SwitchWakerHD Installer  —  github.com/centollOS/SwitchWakerHD
// C# / .NET Framework 4.8 / WinForms dialogs — zero runtime dependency on Win10/11
#pragma warning disable CA1416  // Windows-only APIs — intentional

using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace SwitchWakerHD.Installer;

// ─────────────────────────────────────────────────────────────────────────────
// Multilingual string table
// ─────────────────────────────────────────────────────────────────────────────
static class Str
{
    static readonly bool Es =
        CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "es";

    static readonly Dictionary<string, (string es, string en)> Table = new()
    {
        ["banner1"]        = ("     SwitchWakerHD  -  Instalador Automatico       ", "     SwitchWakerHD  -  Automatic Installer         "),
        ["banner2"]        = ("     Wind Waker HD para Nintendo Switch             ", "     Wind Waker HD for Nintendo Switch              "),
        ["workdir"]        = ("Directorio de trabajo: {0}",                          "Working directory: {0}"),
        ["sec_python"]     = ("Python 3",                                             "Python 3"),
        ["sec_devkit"]     = ("devkitPro Switch Toolchain",                          "devkitPro Switch Toolchain"),
        ["sec_release"]    = ("SwitchWakerHD {0}",                                   "SwitchWakerHD {0}"),
        ["sec_game"]       = ("Tu copia del juego",                                  "Your game dump"),
        ["py_found"]       = ("Python encontrado: {0}",                              "Python found: {0}"),
        ["py_installing"]  = ("Instalando Python 3.12 via winget...",                "Installing Python 3.12 via winget..."),
        ["py_ok"]          = ("Python instalado correctamente.",                     "Python installed successfully."),
        ["py_fail"]        = ("No se pudo instalar Python automaticamente.\nInstala Python 3 desde https://python.org y vuelve a ejecutar este instalador.", "Could not install Python automatically.\nInstall Python 3 from https://python.org and run this installer again."),
        ["py_pkgs"]        = ("Instalando dependencias Python (zstandard, pycryptodome)...", "Installing Python dependencies (zstandard, pycryptodome)..."),
        ["py_pkgs_ok"]     = ("Dependencias Python instaladas.",                     "Python dependencies installed."),
        ["dkp_already"]    = ("devkitPro ya esta instalado.",                        "devkitPro is already installed."),
        ["dkp_downloading"]= ("Descargando instalador de devkitPro v3.0.3...",       "Downloading devkitPro installer v3.0.3..."),
        ["dkp_verifying"]  = ("Verificando integridad del instalador...",            "Verifying installer integrity..."),
        ["dkp_verified"]   = ("Integridad verificada.",                              "Integrity verified."),
        ["dkp_notice"]     = ("El instalador de devkitPro se va a abrir.\n  1. Acepta el acuerdo de licencia\n  2. Deja la ruta por defecto: C:\\devkitPro\n  3. Completa la instalacion y cierra el instalador", "The devkitPro installer is about to open.\n  1. Accept the license agreement\n  2. Keep the default path: C:\\devkitPro\n  3. Complete the installation and close the installer"),
        ["dkp_press_open"] = ("  Presiona ENTER para abrir el instalador...",        "  Press ENTER to open the installer..."),
        ["dkp_manual"]     = ("Si el instalador no ha terminado, completalo ahora.", "If the installer has not finished, complete it now."),
        ["dkp_press_done"] = ("  Presiona ENTER cuando hayas cerrado el instalador...", "  Press ENTER once you have closed the installer..."),
        ["dkp_not_found"]  = ("No se encontro dkp-pacman bajo C:\\devkitPro.\nAsegurate de que devkitPro quedo instalado en C:\\devkitPro.", "dkp-pacman not found under C:\\devkitPro.\nMake sure devkitPro was installed to C:\\devkitPro."),
        ["dkp_searching"]  = ("Buscando dkp-pacman en C:\\devkitPro...",             "Searching for dkp-pacman in C:\\devkitPro..."),
        ["dkp_found_at"]   = ("dkp-pacman encontrado en: {0}",                      "dkp-pacman found at: {0}"),
        ["dkp_installing"] = ("Instalando paquetes Nintendo Switch...",              "Installing Nintendo Switch packages..."),
        ["dkp_pkg_warn"]   = ("Algunos paquetes pueden ya estar instalados. Continuando...", "Some packages may already be installed. Continuing..."),
        ["dkp_ready"]      = ("Toolchain Switch listo.",                             "Switch toolchain ready."),
        ["rel_checking"]   = ("Buscando la ultima version de SwitchWakerHD...",      "Checking for the latest SwitchWakerHD release..."),
        ["rel_found"]      = ("Version encontrada: {0}",                             "Latest version: {0}"),
        ["rel_fallback"]   = ("No se pudo verificar la ultima version, usando {0}",  "Could not check latest version, using {0}"),
        ["rel_already"]    = ("Release {0} ya descargado.",                          "Release {0} already downloaded."),
        ["rel_downloading"]= ("Descargando SwitchWakerHD {0}...",                    "Downloading SwitchWakerHD {0}..."),
        ["rel_verifying"]  = ("Verificando integridad del release...",               "Verifying release integrity..."),
        ["rel_verified"]   = ("Descarga verificada.",                                "Download verified."),
        ["rel_extracting"] = ("Extrayendo archivos...",                              "Extracting files..."),
        ["rel_extracted"]  = ("Extraido en: {0}",                                    "Extracted to: {0}"),
        ["rel_done"]       = ("Release ya extraido.",                                "Release already extracted."),
        ["game_header"]    = ("Necesitas tu dump PERSONAL de Wind Waker HD (USA o EUR, version 0).", "You need your PERSONAL dump of Wind Waker HD (USA or EUR, version 0)."),
        ["game_opt1"]      = ("  [1] Imagen de disco  (.wux / .wud)  — necesita GAME.key y common.key", "  [1] Disc image       (.wux / .wud)  — needs GAME.key and common.key"),
        ["game_opt2"]      = ("  [2] Archivo Cemu     (.wua)          — no necesita claves",             "  [2] Cemu archive     (.wua)          — no keys needed"),
        ["game_opt3"]      = ("  [3] Carpeta extraida (contiene code/, content/, meta/)",                "  [3] Extracted folder (contains code/, content/, meta/)"),
        ["game_prompt"]    = ("Tu eleccion [1/2/3]: ",                               "Your choice [1/2/3]: "),
        ["game_selected"]  = ("Juego seleccionado: {0}",                             "Game selected: {0}"),
        ["game_invalid"]   = ("Opcion invalida. Ejecuta el instalador de nuevo.",    "Invalid option. Run the installer again."),
        ["game_none"]      = ("No se selecciono ningun archivo o carpeta valido.",   "No valid file or folder was selected."),
        ["dlg_title_img"]  = ("Selecciona la imagen del juego",                      "Select the game disc image"),
        ["dlg_filter_img"] = ("Imagen de disco (*.wux;*.wud)|*.wux;*.wud|Todos (*.*)|*.*", "Disc image (*.wux;*.wud)|*.wux;*.wud|All files (*.*)|*.*"),
        ["dlg_title_wua"]  = ("Selecciona el archivo Cemu (.wua)",                   "Select the Cemu archive (.wua)"),
        ["dlg_filter_wua"] = ("Archivo Cemu (*.wua)|*.wua|Todos (*.*)|*.*",          "Cemu archive (*.wua)|*.wua|All files (*.*)|*.*"),
        ["dlg_folder"]     = ("Selecciona la carpeta raiz del juego (contiene code/, content/, meta/)", "Select the game root folder (contains code/, content/, meta/)"),
        ["dlg_type_path"]  = ("Escribe o pega la ruta de la carpeta: ",              "Type or paste the folder path: "),
        ["build_wait"]     = ("Construyendo... esto tarda entre 10 y 30 minutos. No cierres esta ventana.", "Building... this takes 10–30 minutes. Do not close this window."),
        ["build_ok"]       = ("Build completado. Archivos en: {0}",                  "Build complete. Output at: {0}"),
        ["build_fail"]     = ("El build fallo (codigo {0}). Revisa los mensajes de error arriba.", "Build failed (exit code {0}). Check the error messages above."),
        ["sd_header"]      = ("Copiar los archivos a tu tarjeta SD?",                "Copy the files to your SD card?"),
        ["sd_hint"]        = ("Asegurate de que la SD este conectada a esta PC.",    "Make sure your SD card is connected to this PC."),
        ["sd_prompt"]      = ("Letra de unidad de la SD (ej: E) o ENTER para saltar: ", "SD card drive letter (e.g. E) or ENTER to skip: "),
        ["sd_not_found"]   = ("No se encontro la unidad {0}. Copia manualmente:",   "Drive {0} not found. Copy manually:"),
        ["sd_copying"]     = ("Copiando a {0}...",                                   "Copying to {0}..."),
        ["sd_copied"]      = ("Archivos copiados a {0}",                             "Files copied to {0}"),
        ["sd_manual"]      = ("Copia manual requerida:",                             "Manual copy required:"),
        ["sd_src"]         = ("  Origen : {0}",                                      "  Source : {0}"),
        ["sd_dst"]         = ("  Destino: raiz de tu tarjeta SD",                    "  Target : root of your SD card"),
        ["final_title"]    = ("    Instalacion completada exitosamente!           ", "    Installation completed successfully!            "),
        ["next_header"]    = ("Proximos pasos en tu Nintendo Switch:",               "Next steps on your Nintendo Switch:"),
        ["press_close"]    = ("Presiona ENTER para cerrar...",                       "Press ENTER to close..."),
        ["err_not_admin"]  = ("Este instalador debe ejecutarse como Administrador.\nHaz clic derecho en el .exe y elige 'Ejecutar como administrador'.", "This installer must be run as Administrator.\nRight-click the .exe and choose 'Run as administrator'."),
        ["err_download"]   = ("No se pudo descargar: {0}\nVerifica tu conexion a internet.", "Could not download: {0}\nCheck your internet connection."),
        ["err_integrity"]  = ("El archivo descargado no paso la verificacion SHA256.\nElimina la carpeta {0} e intenta de nuevo.", "Downloaded file failed SHA256 verification.\nDelete the folder {0} and try again."),
    };

    public static string Get(string key, params object[] args)
    {
        if (!Table.TryGetValue(key, out var pair)) return $"[{key}]";
        var s = Es ? pair.es : pair.en;
        return args.Length > 0 ? string.Format(s, args) : s;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main program
// ─────────────────────────────────────────────────────────────────────────────
class Program
{
    static readonly string WorkDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "SwitchWakerHD-Build");

    static readonly string DevkitRoot = @"C:\devkitPro";

    static readonly HttpClient Http = CreateHttpClient();
    static HttpClient CreateHttpClient()
    {
        var c = new HttpClient();
        c.Timeout = TimeSpan.FromMinutes(30);
        c.DefaultRequestHeaders.UserAgent.ParseAdd("SwitchWakerHD-Installer/1.0");
        return c;
    }

    static string PythonCmd  = "python";
    static string DkpPacman  = "";
    static string ReleaseVer = "v0.4.3";
    static string ReleaseDir = "";

    // ── Entry point ──────────────────────────────────────────────────────────
    [STAThread]
    static void Main()
    {
        Console.OutputEncoding = System.Text.Encoding.UTF8;
        Console.Title = "SwitchWakerHD Installer";
        Application.EnableVisualStyles();

        PrintBanner();

        if (!IsAdmin()) ExitError(Str.Get("err_not_admin"));

        Directory.CreateDirectory(WorkDir);
        WriteStep(Str.Get("workdir", WorkDir));

        Step1_Python();
        Step2_DevkitPro();
        Step3_Release();
        var (gamePath, buildArg) = Step4_GameDump();
        Step5_Build(gamePath, buildArg);
        StepSD();
        PrintDone();
    }

    // ── Step 1: Python ───────────────────────────────────────────────────────
    static void Step1_Python()
    {
        WriteSection("1/5", Str.Get("sec_python"));

        foreach (var cmd in new[] { "python", "python3", "py" })
        {
            var (code, output) = RunCapture(cmd, "--version");
            if (code == 0 && output.Contains("Python 3."))
            {
                PythonCmd = cmd;
                WriteOK(Str.Get("py_found", output.Trim()));
                goto installPkgs;
            }
        }

        WriteStep(Str.Get("py_installing"));
        if (RunLive("winget", "install --id Python.Python.3.12 --source winget --accept-package-agreements --accept-source-agreements --silent") != 0)
            ExitError(Str.Get("py_fail"));
        RefreshPath();
        WriteOK(Str.Get("py_ok"));

        installPkgs:
        WriteStep(Str.Get("py_pkgs"));
        RunLive(PythonCmd, "-m pip install --quiet zstandard pycryptodome");
        WriteOK(Str.Get("py_pkgs_ok"));
    }

    // ── Step 2: devkitPro ────────────────────────────────────────────────────
    static void Step2_DevkitPro()
    {
        WriteSection("2/5", Str.Get("sec_devkit"));

        WriteStep(Str.Get("dkp_searching"));
        DkpPacman = FindDkpPacman();

        if (string.IsNullOrEmpty(DkpPacman))
        {
            var installer = Path.Combine(WorkDir, "devkitProInstaller.exe");
            WriteStep(Str.Get("dkp_downloading"));
            Download("https://github.com/devkitPro/installer/releases/download/v3.0.3/devkitProUpdater-3.0.3.exe", installer);
            WriteStep(Str.Get("dkp_verifying"));
            VerifySHA256(installer, "038a99dc84f1ca0b52e9e0e074a94a3b0672e6d7bf0988563f0ab0812dcbb38d");
            WriteOK(Str.Get("dkp_verified"));

            Console.WriteLine();
            WriteWarn(Str.Get("dkp_notice"));
            Console.WriteLine();
            Prompt(Str.Get("dkp_press_open"));

            Process.Start(new ProcessStartInfo(installer) { UseShellExecute = true })?.WaitForExit();
            RefreshPath();

            WriteStep(Str.Get("dkp_searching"));
            DkpPacman = FindDkpPacman();

            if (string.IsNullOrEmpty(DkpPacman))
            {
                WriteWarn(Str.Get("dkp_manual"));
                Prompt(Str.Get("dkp_press_done"));
                RefreshPath();
                DkpPacman = FindDkpPacman();
            }
        }
        else
        {
            WriteOK(Str.Get("dkp_already"));
        }

        if (string.IsNullOrEmpty(DkpPacman)) ExitError(Str.Get("dkp_not_found"));
        WriteOK(Str.Get("dkp_found_at", DkpPacman));

        WriteStep(Str.Get("dkp_installing"));
        SetDevkitEnv();
        if (RunLive(DkpPacman, "-Sy --noconfirm switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake") != 0)
            WriteWarn(Str.Get("dkp_pkg_warn"));
        SetDevkitEnv();
        WriteOK(Str.Get("dkp_ready"));
    }

    // ── Step 3: Download release ─────────────────────────────────────────────
    static void Step3_Release()
    {
        WriteStep(Str.Get("rel_checking"));
        try
        {
            var json = Http.GetStringAsync("https://api.github.com/repos/centollOS/SwitchWakerHD/releases/latest").GetAwaiter().GetResult();
            var m = Regex.Match(json, "\"tag_name\":\\s*\"([^\"]+)\"");
            if (m.Success) ReleaseVer = m.Groups[1].Value;
            WriteOK(Str.Get("rel_found", ReleaseVer));
        }
        catch { WriteWarn(Str.Get("rel_fallback", ReleaseVer)); }

        WriteSection("3/5", Str.Get("sec_release", ReleaseVer));

        var zip = Path.Combine(WorkDir, $"SwitchWakerHD-{ReleaseVer}.zip");
        ReleaseDir = Path.Combine(WorkDir, $"SwitchWakerHD-{ReleaseVer}");

        if (!File.Exists(zip))
        {
            WriteStep(Str.Get("rel_downloading", ReleaseVer));
            Download($"https://github.com/centollOS/SwitchWakerHD/releases/download/{ReleaseVer}/SwitchWakerHD-{ReleaseVer}.zip", zip);

            WriteStep(Str.Get("rel_verifying"));
            var sha256File = zip + ".sha256";
            Download($"https://github.com/centollOS/SwitchWakerHD/releases/download/{ReleaseVer}/SwitchWakerHD-{ReleaseVer}.zip.sha256", sha256File);
            var expected = File.ReadAllText(sha256File).Split(' ')[0].Trim().ToUpperInvariant();
            VerifySHA256(zip, expected);
            WriteOK(Str.Get("rel_verified"));
        }
        else { WriteOK(Str.Get("rel_already", ReleaseVer)); }

        if (!Directory.Exists(ReleaseDir))
        {
            WriteStep(Str.Get("rel_extracting"));
            ZipFile.ExtractToDirectory(zip, ReleaseDir);
            WriteOK(Str.Get("rel_extracted", ReleaseDir));
        }
        else { WriteOK(Str.Get("rel_done")); }
    }

    // ── Step 4: Game dump ────────────────────────────────────────────────────
    static (string gamePath, string buildArg) Step4_GameDump()
    {
        WriteSection("4/5", Str.Get("sec_game"));
        Console.WriteLine();
        Gray("  " + Str.Get("game_header"));
        Console.WriteLine();
        White(Str.Get("game_opt1"));
        White(Str.Get("game_opt2"));
        White(Str.Get("game_opt3"));
        Console.WriteLine();
        Console.ForegroundColor = ConsoleColor.White;
        Console.Write("  " + Str.Get("game_prompt"));
        Console.ResetColor();

        var choice = Console.ReadLine()?.Trim() ?? "";

        string gamePath = "", buildArg = "";

        if (choice == "1")
        {
            using var dlg = new OpenFileDialog { Title = Str.Get("dlg_title_img"), Filter = Str.Get("dlg_filter_img") };
            if (dlg.ShowDialog() == DialogResult.OK) gamePath = dlg.FileName;
            buildArg = "--image";
        }
        else if (choice == "2")
        {
            using var dlg = new OpenFileDialog { Title = Str.Get("dlg_title_wua"), Filter = Str.Get("dlg_filter_wua") };
            if (dlg.ShowDialog() == DialogResult.OK) gamePath = dlg.FileName;
            buildArg = "--wua";
        }
        else if (choice == "3")
        {
            using var dlg = new FolderBrowserDialog
            {
                Description = Str.Get("dlg_folder"),
                AutoUpgradeEnabled = true,
                ShowNewFolderButton = false
            };
            if (dlg.ShowDialog() == DialogResult.OK) gamePath = dlg.SelectedPath;

            if (string.IsNullOrWhiteSpace(gamePath))
            {
                Console.WriteLine();
                WriteWarn("Dialog returned no path — type it manually:");
                Console.ForegroundColor = ConsoleColor.White;
                Console.Write("  " + Str.Get("dlg_type_path"));
                Console.ResetColor();
                gamePath = (Console.ReadLine() ?? "").Trim().Trim('"').Trim('\'');
            }
            buildArg = "--game-dir";
        }
        else
        {
            ExitError(Str.Get("game_invalid"));
        }

        if (string.IsNullOrWhiteSpace(gamePath) ||
            (!File.Exists(gamePath) && !Directory.Exists(gamePath)))
            ExitError(Str.Get("game_none"));

        WriteOK(Str.Get("game_selected", gamePath));
        return (gamePath, buildArg);
    }

    // ── Step 5: Build ────────────────────────────────────────────────────────
    static void Step5_Build(string gamePath, string buildArg)
    {
        WriteSection("5/5", "Build");
        Console.WriteLine();
        Console.ForegroundColor = ConsoleColor.Blue;
        Console.WriteLine("  =================================================");
        Console.ForegroundColor = ConsoleColor.Yellow;
        Console.WriteLine("  " + Str.Get("build_wait"));
        Console.ForegroundColor = ConsoleColor.Blue;
        Console.WriteLine("  =================================================");
        Console.ResetColor();
        Console.WriteLine();

        var script = Path.Combine(ReleaseDir, "tools", "switch", "make_sd.py");
        var rc = RunLive(PythonCmd, $"\"{script}\" {buildArg} \"{gamePath}\"", ReleaseDir);
        if (rc != 0) ExitError(Str.Get("build_fail", rc));

        WriteOK(Str.Get("build_ok", Path.Combine(ReleaseDir, "build", "sd")));
    }

    // ── SD copy ──────────────────────────────────────────────────────────────
    static void StepSD()
    {
        var sdOut = Path.Combine(ReleaseDir, "build", "sd");
        Console.WriteLine();
        White("  " + Str.Get("sd_header"));
        Gray("  " + Str.Get("sd_hint"));
        Console.WriteLine();
        Console.ForegroundColor = ConsoleColor.White;
        Console.Write("  " + Str.Get("sd_prompt"));
        Console.ResetColor();

        var letter = Console.ReadLine()?.Trim() ?? "";
        if (letter.Length == 1 && char.IsLetter(letter[0]))
        {
            var drive = $"{char.ToUpper(letter[0])}:\\";
            if (!Directory.Exists(drive))
            {
                WriteWarn(Str.Get("sd_not_found", drive));
                Gray($"  {sdOut}");
            }
            else
            {
                WriteStep(Str.Get("sd_copying", drive));
                RunLive("robocopy", $"\"{sdOut}\" \"{drive}\" /E /NFL /NDL /NJH /NJS /NC /NS");
                WriteOK(Str.Get("sd_copied", drive));
            }
        }
        else
        {
            Console.WriteLine();
            WriteWarn(Str.Get("sd_manual"));
            Gray(Str.Get("sd_src", sdOut));
            Gray(Str.Get("sd_dst"));
        }
    }

    // ── Console helpers ──────────────────────────────────────────────────────
    static void PrintBanner()
    {
        Console.Clear();
        Console.WriteLine();
        Console.ForegroundColor = ConsoleColor.Blue;
        Console.WriteLine("  =================================================");
        Console.ForegroundColor = ConsoleColor.White;
        Console.WriteLine(Str.Get("banner1"));
        Console.ForegroundColor = ConsoleColor.Gray;
        Console.WriteLine(Str.Get("banner2"));
        Console.ForegroundColor = ConsoleColor.Blue;
        Console.WriteLine("  =================================================");
        Console.ResetColor();
        Console.WriteLine();
    }

    static void PrintDone()
    {
        Console.WriteLine();
        Console.ForegroundColor = ConsoleColor.Green;
        Console.WriteLine("  =================================================");
        Console.WriteLine(Str.Get("final_title"));
        Console.WriteLine("  =================================================");
        Console.ResetColor();
        Console.WriteLine();
        White("  " + Str.Get("next_header"));
        var steps = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "es"
            ? new[] { "1. Inserta la SD con los archivos copiados", "2. Abre el Homebrew Menu (manten R al lanzar cualquier juego)", "3. Selecciona SwitchWakerHD", "4. Primera vez: compilara graficos (~20 min), es normal" }
            : new[] { "1. Insert the SD card with the copied files", "2. Open Homebrew Menu (hold R while launching any game)", "3. Select SwitchWakerHD", "4. First launch: graphics will compile (~20 min), this is normal" };
        foreach (var s in steps) Gray($"   {s}");
        Console.WriteLine();
        Prompt(Str.Get("press_close"));
    }

    static void WriteSection(string n, string title)
    {
        Console.WriteLine();
        Console.ForegroundColor = ConsoleColor.White;
        Console.WriteLine($"  ---- [{n}] {title} ----");
        Console.ResetColor();
    }

    static void WriteStep(string msg)  { Console.ForegroundColor = ConsoleColor.Cyan;   Console.WriteLine($"  >> {msg}");    Console.ResetColor(); }
    static void WriteOK(string msg)    { Console.ForegroundColor = ConsoleColor.Green;  Console.WriteLine($"  [OK] {msg}");  Console.ResetColor(); }
    static void WriteWarn(string msg)  { Console.ForegroundColor = ConsoleColor.Yellow; Console.WriteLine($"  [!!] {msg}");  Console.ResetColor(); }
    static void White(string msg)      { Console.ForegroundColor = ConsoleColor.White;  Console.WriteLine(msg);              Console.ResetColor(); }
    static void Gray(string msg)       { Console.ForegroundColor = ConsoleColor.Gray;   Console.WriteLine(msg);              Console.ResetColor(); }
    static void Prompt(string msg)     { Console.ForegroundColor = ConsoleColor.White;  Console.Write(msg);                  Console.ResetColor(); Console.ReadLine(); }

    [System.Diagnostics.CodeAnalysis.DoesNotReturn]
    static void ExitError(string msg)
    {
        Console.WriteLine();
        Console.ForegroundColor = ConsoleColor.Red;
        Console.WriteLine($"  [ERROR] {msg}");
        Console.ResetColor();
        Console.WriteLine();
        Prompt(Str.Get("press_close"));
        Environment.Exit(1);
    }

    // ── System helpers ───────────────────────────────────────────────────────
    static bool IsAdmin() =>
        new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);

    static string FindDkpPacman()
    {
        var candidates = new[]
        {
            Path.Combine(DevkitRoot, "devkitpro-pacman", "bin", "dkp-pacman.exe"),
            Path.Combine(DevkitRoot, "devkitpro-pacman", "usr", "bin", "pacman.exe"),
            Path.Combine(DevkitRoot, "msys2", "usr", "bin", "pacman.exe"),
            Path.Combine(DevkitRoot, "pacman", "usr", "bin", "pacman.exe"),
        };
        foreach (var c in candidates)
            if (File.Exists(c)) return c;

        if (!Directory.Exists(DevkitRoot)) return "";
        foreach (var name in new[] { "dkp-pacman.exe", "pacman.exe" })
        {
            var hit = Directory.EnumerateFiles(DevkitRoot, name, SearchOption.AllDirectories).FirstOrDefault();
            if (hit != null) return hit;
        }
        return "";
    }

    static void SetDevkitEnv()
    {
        var path = Environment.GetEnvironmentVariable("PATH") ?? "";
        Environment.SetEnvironmentVariable("DEVKITPRO", DevkitRoot);
        Environment.SetEnvironmentVariable("DEVKITARM", Path.Combine(DevkitRoot, "devkitARM"));
        Environment.SetEnvironmentVariable("DEVKITPPC", Path.Combine(DevkitRoot, "devkitPPC"));
        Environment.SetEnvironmentVariable("PATH", $@"{DevkitRoot}\tools\bin;{DevkitRoot}\devkitARM\bin;" + path);
    }

    static void RefreshPath()
    {
        var machine = Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.Machine) ?? "";
        var user    = Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.User) ?? "";
        Environment.SetEnvironmentVariable("PATH", machine + ";" + user);
    }

    static void Download(string url, string dest)
    {
        using var response = Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
        if (!response.IsSuccessStatusCode) ExitError(Str.Get("err_download", url));

        var total = response.Content.Headers.ContentLength ?? -1L;
        using var src  = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();
        using var file = File.Create(dest);
        var buf = new byte[81920];
        long done = 0; int read;
        while ((read = src.Read(buf, 0, buf.Length)) > 0)
        {
            file.Write(buf, 0, read);
            done += read;
            if (total > 0)
                Console.Write($"\r  >> {done / 1024} KB / {total / 1024} KB  ({done * 100 / total}%)   ");
        }
        Console.WriteLine();
    }

    static void VerifySHA256(string file, string expected)
    {
        using var sha    = SHA256.Create();
        using var stream = File.OpenRead(file);
        var hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToUpperInvariant();
        if (!string.Equals(hash, expected, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(file);
            ExitError(Str.Get("err_integrity", WorkDir));
        }
    }

    // Runs a process and streams its output live to the console
    static int RunLive(string exe, string args, string? workDir = null)
    {
        try
        {
            var psi = new ProcessStartInfo(exe, args)
            {
                UseShellExecute  = false,
                WorkingDirectory = workDir ?? WorkDir,
            };
            using var proc = Process.Start(psi) ?? throw new Exception("Failed to start process");
            proc.WaitForExit();
            return proc.ExitCode;
        }
        catch { return -1; }
    }

    // Runs a process silently and captures its output
    static (int code, string output) RunCapture(string exe, string args)
    {
        try
        {
            var psi = new ProcessStartInfo(exe, args)
            {
                UseShellExecute        = false,
                RedirectStandardOutput = true,
                RedirectStandardError  = true,
            };
            using var proc = Process.Start(psi)!;
            var output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
            proc.WaitForExit();
            return (proc.ExitCode, output);
        }
        catch { return (-1, ""); }
    }
}
