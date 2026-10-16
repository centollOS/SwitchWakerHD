// The install steps, run off the UI thread: output goes to events, and failures throw InstallException.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace SwitchWakerHD.Installer;

enum StepState { Pending, Running, Done, Failed }
enum LogKind { Info, Ok, Warn, Error, Output }

sealed class InstallException : Exception
{
    public InstallException(string message) : base(message) { }
}

sealed class InstallEngine
{
    public static readonly string WorkDir = ChooseWorkDir();

    // %USERPROFILE%\SwitchWakerHD-Build, unless the user name has a space or a non-ASCII letter ("JAM Visuals",
    // issue #30): the build tools do not all cope with those, so then a folder at the root of the system drive
    // (C:\SwitchWakerHD-Build), which standard users may create. Back to the profile if that cannot be created.
    // Also there when the profile path is long: the release is unpacked two folders down and the deepest object
    // file adds ~160 characters, and devkitPro's gcc cannot open paths over 260 (MAX_PATH).
    const int MaxWorkDirLength = 45;
    static string ChooseWorkDir()
    {
        var inProfile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "SwitchWakerHD-Build");
        if (Regex.IsMatch(inProfile, @"^[A-Za-z0-9_.:\\-]+$") && inProfile.Length <= MaxWorkDirLength)
            return inProfile;
        var atRoot = Path.Combine(Path.GetPathRoot(Environment.SystemDirectory) ?? @"C:\", "SwitchWakerHD-Build");
        try
        {
            Directory.CreateDirectory(atRoot);
            return atRoot;
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            return inProfile;
        }
    }
    // devkitPro's folder: C:\devkitPro (its installer's default, what the wizard asks for), or X:\devkitPro on
    // another drive when it was installed there. Looked up on each use: it appears during the devkitPro step.
    static string DevkitRoot => FindDevkitRoot();
    static string FindDevkitRoot()
    {
        const string fallback = @"C:\devkitPro";
        if (File.Exists(Path.Combine(fallback, @"msys2\usr\bin\bash.exe"))) return fallback;
        try
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType != DriveType.Fixed || !drive.IsReady) continue;
                var root = Path.Combine(drive.RootDirectory.FullName, "devkitPro");
                if (File.Exists(Path.Combine(root, @"msys2\usr\bin\bash.exe"))) return root;
            }
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
        return fallback;
    }

    public const int StepPython = 0, StepDevkit = 1, StepRelease = 2, StepBuild = 3;

    public event Action<string, LogKind>? Log;
    public event Action<int, StepState>? StepChanged;
    public event Action<string>? Status;
    public event Action<double?>? Progress;          // 0..1, or null for "busy, no estimate"
    // Shows a notice (title, text, button) and blocks this thread until the user presses the button.
    public Action<string, string, string>? AskUser;

    public string GamePath = "";
    public string GameArg  = "";                      // --image, --wua or --game-dir
    // Optional: the console's prod.keys, for the HOME-screen forwarder (.nsp). Only with it are Git, Pillow
    // and MSYS2's gcc installed (INSTALL.md, "HOME-screen icon"); without it make_sd.py gets --no-forwarder.
    public string? ForwarderKeys;
    // Optional: the player's GameCube Wind Waker (disc image or folder), for the GameCube sea minimap: make_sd.py
    // downloads the mod from the mod catalogue and builds it into the game (INSTALL.md, "Optional: code mods").
    public string? MinimapGame;
    public const string MinimapMod = "gc-minimap";

    public string ReleaseDir { get; private set; } = "";
    public string SdOut => Path.Combine(ReleaseDir, "build", "sd");

    string pythonCmd  = "python";
    string pacman     = "";
    string releaseVer = "";
    CancellationToken ct;
    ProcessJob? currentJob;   // the running process and all it started; KillCurrent stops them

    static readonly HttpClient Http = CreateHttpClient();
    static HttpClient CreateHttpClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("SwitchWakerHD-Installer/1.0");
        return c;
    }

    public void Run(CancellationToken token)
    {
        ct = token;
        Directory.CreateDirectory(WorkDir);
        Info(Str.Get("welcome_dir", WorkDir));
        Preflight();
        RunStep(StepPython,  Python);
        RunStep(StepDevkit,  DevkitPro);
        RunStep(StepRelease, Release);
        RunStep(StepBuild,   Build);
    }

    void RunStep(int step, Action body)
    {
        ct.ThrowIfCancellationRequested();
        StepChanged?.Invoke(step, StepState.Running);
        Progress?.Invoke(null);
        try { body(); }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is InvalidDataException ||
                                  e is HttpRequestException || (e is OperationCanceledException && !ct.IsCancellationRequested))
        {
            // A file or network error with no message of ours (a timeout is a cancel nobody asked for): say which
            // step it was and what probably caused it.
            StepChanged?.Invoke(step, StepState.Failed);
            Log?.Invoke(e.ToString(), LogKind.Error);
            var key = e is HttpRequestException || e is OperationCanceledException ? "err_network" : "err_io";
            throw new InstallException(Str.Get(key, Str.Get(StepKeys[step]), e.Message));
        }
        catch { StepChanged?.Invoke(step, StepState.Failed); throw; }
        StepChanged?.Invoke(step, StepState.Done);
    }

    public static readonly string[] StepKeys = { "step_python", "step_devkit", "step_release", "step_build" };

    // ── Before anything: the game, its keys and the disk ────────────────────
    // What make_sd would only stop on after the downloads and the toolchain (#28: the disc key was not where it
    // looks), checked up front.
    void Preflight()
    {
        Info(Str.Get("pre_game", GameArg, GamePath));
        if (GameArg == "--game-dir" ? !Directory.Exists(GamePath) : !File.Exists(GamePath))
            throw new InstallException(Str.Get("pre_no_game", GamePath));
        if (GameArg == "--image" && CheckImageKeys(GamePath) is string keys)
            throw new InstallException(keys);

        if (MinimapGame != null)
        {
            Info(Str.Get("mm_pre", MinimapGame));
            if (!File.Exists(MinimapGame) && !Directory.Exists(MinimapGame))
                throw new InstallException(Str.Get("mm_no_game", MinimapGame));
        }

        CheckClock();

        // OneDrive "online-only" (Files On-Demand): the whole file is downloaded when make_sd reads it
        if (GameArg != "--game-dir" && (File.GetAttributes(GamePath) & (FileAttributes.Offline | RecallOnDataAccess)) != 0)
            Warn(Str.Get("pre_cloud", GamePath));

        // The extracted game twice (build/sd-game and the SD folder), the generated code, the objects: ~9 GB
        // the first time; a re-run reuses most of it.
        var rerun = Directory.Exists(WorkDir) && Directory.GetDirectories(WorkDir, "SwitchWakerHD-v*").Length > 0;
        var needGb = rerun ? 5 : 10;
        try
        {
            var drive = new DriveInfo(Path.GetPathRoot(WorkDir)!);
            var freeGb = drive.AvailableFreeSpace / 1e9;
            Info(Str.Get("pre_space", drive.Name, freeGb.ToString("0.0")));
            if (freeGb < needGb) throw new InstallException(Str.Get("pre_no_space", drive.Name, freeGb.ToString("0.0"), needGb));
        }
        catch (ArgumentException) { }   // a drive DriveInfo cannot describe (network share): no check
    }

    const FileAttributes RecallOnDataAccess = (FileAttributes)0x400000;

    // pacman and pip fail with "certificate has expired" for every server when the PC's clock is far off (a log
    // in #30): compare it with GitHub's. Being offline is only a warning here: a re-run may need no download.
    void CheckClock()
    {
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Head, "https://github.com");
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromSeconds(20));
            using var resp = Http.SendAsync(req, timeout.Token).GetAwaiter().GetResult();
            var date = resp.Headers.Date;
            if (date != null && Math.Abs((DateTimeOffset.UtcNow - date.Value).TotalHours) > 24)
                Warn(Str.Get("pre_clock", DateTime.Now.ToString("g"), date.Value.ToLocalTime().ToString("g")));
        }
        catch (HttpRequestException e) when (e.InnerException is WebException we &&
                                             (we.Status == WebExceptionStatus.TrustFailure || we.Status == WebExceptionStatus.SecureChannelFailure))
        {
            throw new InstallException(Str.Get("pre_tls", e.InnerException.Message));
        }
        catch (Exception e) when (e is HttpRequestException || (e is OperationCanceledException && !ct.IsCancellationRequested))
        {
            Warn(Str.Get("pre_offline", e.InnerException?.Message ?? e.Message));
        }
    }

    // The keys make_sd needs for a disc image (tools/wudextract.py): the disc key as <image name>.key next to the
    // image, and the Wii U common key as common.key next to it (or WIIU_COMMON_KEY); each 16 bytes or 32 hex digits.
    // Null when both are there, else what is wrong and exactly which file to put where.
    internal static string? CheckImageKeys(string image)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(image)) ?? "";
        var discKey = Path.Combine(dir, Path.GetFileNameWithoutExtension(image) + ".key");
        var common = Path.Combine(dir, "common.key");
        if (!File.Exists(discKey))
        {
            if (File.Exists(discKey + ".txt")) return Str.Get("key_txt", discKey + ".txt", Path.GetFileName(discKey));
            // a key under another name ("game.key", "title.key"): say which to rename. (On Windows "*.key" also
            // matches prod.keys, hence the extension check.)
            var others = Directory.Exists(dir)
                ? Directory.GetFiles(dir, "*.key").Where(f => Path.GetExtension(f).Equals(".key", StringComparison.OrdinalIgnoreCase) &&
                                                              !Path.GetFileName(f).Equals("common.key", StringComparison.OrdinalIgnoreCase)).ToList()
                : new List<string>();
            return others.Count == 1 ? Str.Get("key_rename", others[0], Path.GetFileName(discKey))
                                     : Str.Get("key_missing", discKey);
        }
        if (!KeyFileOk(File.ReadAllBytes(discKey))) return Str.Get("key_bad", discKey);
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("WIIU_COMMON_KEY"))) return null;
        if (!File.Exists(common))
            return File.Exists(common + ".txt") ? Str.Get("key_txt", common + ".txt", "common.key") : Str.Get("common_missing", common);
        if (!KeyFileOk(File.ReadAllBytes(common))) return Str.Get("key_bad", common);
        return null;
    }

    // wudextract.parse_key: 16 raw bytes, or 32 hex digits with whitespace around them
    internal static bool KeyFileOk(byte[] data)
    {
        if (data.Length == 16) return true;
        var text = Encoding.ASCII.GetString(data.Where(b => b < 0x80).ToArray());
        if (text.Length != data.Length) return false;
        text = text.Trim(' ', '\t', '\r', '\n', '\v', '\f');
        return text.Length == 32 && text.All(Uri.IsHexDigit);
    }

    // One argument for a Windows command line: in quotes, with trailing backslashes doubled so that a folder such
    // as D:\ does not escape the closing quote. (Windows paths cannot contain quotes.)
    internal static string Quote(string arg)
    {
        var trailing = arg.Length - arg.TrimEnd('\\').Length;
        return "\"" + arg + new string('\\', trailing) + "\"";
    }

    // ── Python ───────────────────────────────────────────────────────────────
    void Python()
    {
        if (!FindPython())
        {
            Say(Str.Get("py_installing"));
            // PrependPath: winget's own silent switches leave python.exe off PATH.
            // -1: winget did not start (older Windows 10, or its App Execution Alias turned off).
            var rc = Exec("winget", "install --id Python.Python.3.12 --source winget --accept-package-agreements " +
                                    "--accept-source-agreements --silent --override \"/quiet InstallAllUsers=1 PrependPath=1 Include_launcher=1\"",
                          check: false);
            RefreshPath();
            if (!FindPython()) throw new InstallException(Str.Get(rc == -1 ? "py_no_winget" : "py_fail"));
        }
        Say(Str.Get("py_pkgs"));
        var pkgs = "zstandard pycryptodome" + (ForwarderKeys != null ? " pillow" : "");
        if (Exec(pythonCmd, "-m pip install --disable-pip-version-check " + pkgs, check: false) != 0)
            throw new InstallException(Str.Get("py_pkgs_fail", pkgs));
        if (MinimapGame != null && IsZstdRvz(MinimapGame)) Python314();
    }

    // A Dolphin .rvz with Zstandard compression (its default): the minimap's map tool reads it with Python 3.14's own
    // compression.zstd. 3.14 goes next to the Python above, reached through the py launcher (py -3.14), which
    // make_sd.py's tools/switch/guest_mods.py looks for; the build keeps the Python above.
    internal static bool IsZstdRvz(string path)
    {
        try
        {
            if (!File.Exists(path)) return false;
            var head = new byte[0x50];
            using (var f = File.OpenRead(path))
                if (f.Read(head, 0, head.Length) != head.Length) return false;
            if (head[0] != 'R' || head[1] != 'V' || head[2] != 'Z' || head[3] != 1) return false;
            return head[0x4C] == 0 && head[0x4D] == 0 && head[0x4E] == 0 && head[0x4F] == 5;   // disc struct: compression
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { return false; }
    }

    bool HasZstdPython()
    {
        foreach (var (cmd, args) in new[] { ("py", "-3.14 "), ("py", "-3 "), (pythonCmd, "") })
            if (Capture(cmd, args + "-c \"import compression.zstd;print('ZSTD')\"").output.Contains("ZSTD")) return true;
        return false;
    }

    void Python314()
    {
        if (HasZstdPython()) return;
        Say(Str.Get("mm_py_installing"));
        var rc = Exec("winget", "install --id Python.Python.3.14 --source winget --accept-package-agreements " +
                                "--accept-source-agreements --silent --override \"/quiet InstallAllUsers=1 PrependPath=0 Include_launcher=1\"",
                      check: false);
        RefreshPath();
        if (!HasZstdPython()) throw new InstallException(Str.Get(rc == -1 ? "py_no_winget" : "mm_py_fail"));
        Ok(Str.Get("mm_py_ok"));
    }

    bool FindPython()
    {
        var candidates = new List<string> { "python", "py", "python3" };
        foreach (var root in new[] {
                     Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                     Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "Python") })
        {
            if (!Directory.Exists(root)) continue;
            candidates.AddRange(Directory.GetDirectories(root, "Python3*")
                .OrderByDescending(d => d)
                .Select(d => Path.Combine(d, "python.exe"))
                .Where(File.Exists));
        }
        // 3.8 or newer (older ones get no pip wheels for zstandard and pycryptodome) and 64-bit (translating the
        // game code needs more memory than a 32-bit process has). The Microsoft Store's "python" stub prints nothing.
        foreach (var cmd in candidates)
        {
            var (code, output) = Capture(cmd, "-c \"import sys,struct;print('PY',sys.version_info[0],sys.version_info[1],struct.calcsize('P')*8)\"");
            var m = Regex.Match(output, @"PY (\d+) (\d+) (\d+)");
            if (code != 0 || !m.Success) continue;
            int major = int.Parse(m.Groups[1].Value), minor = int.Parse(m.Groups[2].Value), bits = int.Parse(m.Groups[3].Value);
            var desc = $"Python {major}.{minor} ({bits}-bit, {cmd})";
            if (major != 3 || minor < 8 || bits != 64) { Info(Str.Get("py_unsuitable", desc)); continue; }
            pythonCmd = cmd;
            Ok(Str.Get("py_found", desc));
            return true;
        }
        return false;
    }

    // ── devkitPro ────────────────────────────────────────────────────────────
    void DevkitPro()
    {
        Say(Str.Get("dkp_searching"));
        pacman = FindPacman();

        if (pacman == "")
        {
            var installer = Path.Combine(WorkDir, "devkitProInstaller.exe");
            Say(Str.Get("dkp_downloading"));
            Download("https://github.com/devkitPro/installer/releases/download/v3.0.3/devkitProUpdater-3.0.3.exe", installer);
            Say(Str.Get("dkp_verifying"));
            VerifySha256(installer, "038a99dc84f1ca0b52e9e0e074a94a3b0672e6d7bf0988563f0ab0812dcbb38d");

            AskUser?.Invoke(Str.Get("dkp_notice_h"), Str.Get("dkp_notice"), Str.Get("dkp_open"));
            Say(Str.Get("dkp_wait"));
            Progress?.Invoke(null);
            // Not killOnClose: whatever the updater leaves running (a pacman still installing) must finish.
            using (var job = new ProcessJob(killOnClose: false))
            using (var p = Process.Start(new ProcessStartInfo(installer) { UseShellExecute = true }))
            {
                if (p != null) job.Add(p);
                currentJob = job;
                try
                {
                    while (p != null && !p.WaitForExit(500))
                        if (ct.IsCancellationRequested) { job.Terminate(); ct.ThrowIfCancellationRequested(); }
                }
                finally { currentJob = null; }
            }
            RefreshPath();
            pacman = FindPacman();

            if (pacman == "")
            {
                AskUser?.Invoke(Str.Get("dkp_manual_h"), Str.Get("dkp_manual"), Str.Get("btn_continue"));
                pacman = FindPacman();
                if (pacman == "") throw new InstallException(Str.Get("dkp_not_found"));
            }
        }
        Ok(Str.Get("dkp_found_at", pacman));

        if (ForwarderKeys != null) ForwarderGit();

        Say(Str.Get("dkp_installing"));
        SetDevkitEnv();
        ClearStalePacmanLock();
        // cmake and make (MSYS2's): build.sh runs them, and no devkitPro package brings them (issue #28).
        // gcc (MSYS2's, for the PC): the forwarder script compiles hacBrewPack, the NSP packer, with it.
        // Only when missing: --needed still upgrades an installed one to the package list's version, without its
        // libraries (cmake 4.0.2 -> 4.4.4 without msys-jsoncpp-27.dll, so cmake no longer started). A partial
        // update of MSYS2 (RepairMsys mends one when the tools then don't start).
        var packages = "switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake";
        foreach (var tool in ForwarderKeys != null ? new[] { "cmake", "make", "gcc" } : new[] { "cmake", "make" })
            if (!File.Exists(Path.Combine(DevkitRoot, @"msys2\usr\bin", tool + ".exe"))) packages += " " + tool;
        var output = new List<string>();
        string? reason = null;
        for (int attempt = 1; ; attempt++)
        {
            output.Clear();
            if (Exec(pacman, "-Sy --needed --noconfirm " + packages, check: false, onLine: output.Add) == 0) { reason = null; break; }
            reason = PacmanFailure(output);
            // a network hiccup may pass; a 403, an expired certificate or a lock will not
            if (attempt == 2 || reason != "dkp_why_net") break;
            Warn(Str.Get("dkp_retry"));
            for (int i = 0; i < 10; i++) { ct.ThrowIfCancellationRequested(); Thread.Sleep(1000); }
            ClearStalePacmanLock();
        }
        if (reason != null)
        {
            // pkg.devkitpro.org often answers 403 (Cloudflare, devkitPro/pacman#63) and the whole -Sy gives up
            // without installing anything. The package lists it did refresh, and the ones from earlier runs,
            // are still on disk: install from those.
            Warn(Str.Get("dkp_pkg_warn") + " " + Str.Get(reason));
            ClearStalePacmanLock();
            Exec(pacman, "-S --needed --noconfirm " + packages, check: false);
        }
        CheckDevkitTools(reason);
    }

    // Why pacman failed, from its output: a Str key that explains it to the player.
    internal static string PacmanFailure(IEnumerable<string> output)
    {
        var all = string.Join("\n", output);
        bool Has(string pattern) => Regex.IsMatch(all, pattern, RegexOptions.IgnoreCase);
        return Has(@"returned error: 403")                                     ? "dkp_why_403" :
               Has(@"certificate has expired|certificate is not yet valid|SSL certificate") ? "dkp_why_ssl" :
               Has(@"unable to lock database")                                 ? "dkp_why_lock" :
               Has(@"returned error: 429")                                     ? "dkp_why_429" :
               Has(@"Could not resolve host|timed out|Failed to connect|Connection reset|returned error: 5\d\d|Recv failure") ? "dkp_why_net" :
               Has(@"signature .* (unknown trust|is invalid)|invalid or corrupted package|keyring") ? "dkp_why_sig" :
               "dkp_why_other";
    }

    // What the build needs, by the files it runs or links. A missing one stops here, with the reason,
    // instead of after the download and two build stages (issue #28).
    void CheckDevkitTools(string? pacmanFailure)
    {
        var needed = new (string name, string file)[]
        {
            ("msys2",       @"msys2\usr\bin\bash.exe"),
            ("cmake",       @"msys2\usr\bin\cmake.exe"),
            ("make",        @"msys2\usr\bin\make.exe"),
            ("devkitA64",   @"devkitA64\bin\aarch64-none-elf-gcc.exe"),
            ("libnx",       @"libnx\lib\libnx.a"),
            ("switch-cmake", @"cmake\Switch.cmake"),
            ("switch-zlib", @"portlibs\switch\lib\libz.a"),
            ("switch-lz4",  @"portlibs\switch\lib\liblz4.a"),
        };
        var missing = needed.Where(t => !File.Exists(Path.Combine(DevkitRoot, t.file))).Select(t => t.name).ToList();
        if (missing.Count > 0)
            throw new InstallException(Str.Get("dkp_missing", string.Join(", ", missing)) + "\n\n" + Str.Get(pacmanFailure ?? "dkp_why_other"));

        // There is not enough: they must also start. An MSYS2 program whose library is missing (cmake installed
        // by a partial update) exits without a word, which is how build.sh stopped silently in #28.
        var bash = Path.Combine(DevkitRoot, @"msys2\usr\bin\bash.exe");
        var env = MsysEnv();
        const string probe = "set -e; command -v cmake make; cmake --version; make --version; $DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc --version";
        if (Exec(bash, "-l -c \"" + probe + "\"", check: false, env: env) != 0)
        {
            Exec(bash, "-l -c \"ldd /usr/bin/cmake.exe /usr/bin/make.exe\"", check: false, env: env);   // for the log: what is missing
            var updateFailure = RepairMsys(bash, env);
            if (Exec(bash, "-l -c \"" + probe + "\"", check: false, env: env) != 0)
            {
                Exec(bash, "-l -c \"ldd /usr/bin/cmake.exe /usr/bin/make.exe\"", check: false, env: env);
                throw new InstallException(updateFailure != null
                    ? Str.Get("dkp_update_fail") + "\n\n" + Str.Get(updateFailure)
                    : Str.Get("dkp_broken"));
            }
            Ok(Str.Get("dkp_repaired"));
        }

        if (ForwarderKeys != null && !File.Exists(Path.Combine(DevkitRoot, @"msys2\usr\bin\gcc.exe")))
        {
            Warn(Str.Get("fwd_gcc_fail"));
            ForwarderKeys = null;
        }
        Ok(Str.Get("dkp_tools_ok"));
    }

    // The tools are installed but don't start: a partial update of MSYS2 (a program newer than the libraries it
    // needs, e.g. cmake from a package list newer than the rest: #28, case 22 of the Windows tests). Mended here
    // rather than asking the player: a full update of MSYS2's own packages, from MSYS2's mirrors only. devkitPro's
    // repositories (often a 403, which stops a whole -Syu) are left out of a copy of pacman.conf: none of the
    // broken programs come from them. Up to three passes: the first one may update only MSYS2's core and end
    // the shell it runs in; the last one finds nothing left to do. Returns why the update failed (a Str key),
    // or null when a pass ended well.
    string? RepairMsys(string bash, IDictionary<string, string> env)
    {
        Say(Str.Get("dkp_repairing"));
        var conf = Path.Combine(DevkitRoot, @"msys2\etc\pacman.conf");
        var copy = Path.Combine(DevkitRoot, @"msys2\tmp\wwhd-msys-only.conf");   // /tmp/wwhd-msys-only.conf in MSYS2
        var kept = new List<string>();
        var dkp = false;
        foreach (var line in File.ReadAllLines(conf))
        {
            var t = line.Trim();
            if (t.StartsWith("[") && t.EndsWith("]")) dkp = t.StartsWith("[dkp", StringComparison.OrdinalIgnoreCase);
            if (!dkp) kept.Add(line);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
        File.WriteAllLines(copy, kept);
        string? failure = null;
        var output = new List<string>();
        for (int pass = 1; pass <= 3; pass++)
        {
            ct.ThrowIfCancellationRequested();
            ClearStalePacmanLock();
            output.Clear();
            var rc = Exec(bash, "-l -c \"pacman --config /tmp/wwhd-msys-only.conf -Syu --noconfirm\"", check: false, env: env,
                          onLine: output.Add);
            if (rc == 0)
            {
                failure = null;
                if (pass >= 2) break;
                continue;
            }
            failure = PacmanFailure(output);
            if (pass == 3) break;
            // the mirrors limit how often one address may download (HTTP 429): give them a minute
            var wait = failure == "dkp_why_429" ? 60 : failure == "dkp_why_net" ? 10 : 0;
            if (wait > 0) Warn(Str.Get(wait == 60 ? "dkp_retry_429" : "dkp_retry"));
            for (int i = 0; i < wait; i++) { ct.ThrowIfCancellationRequested(); Thread.Sleep(1000); }
        }
        try { File.Delete(copy); } catch (IOException) { }
        return failure;
    }

    static string FindPacman()
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

    // A pacman killed mid-sync (or one the devkitPro updater left running) keeps db.lck, and the next
    // -Sy fails with "unable to lock database". Remove the lock only once no pacman is running.
    void ClearStalePacmanLock()
    {
        static bool PacmanRunning() =>
            Process.GetProcessesByName("pacman").Length + Process.GetProcessesByName("dkp-pacman").Length > 0;

        if (PacmanRunning())
        {
            Say(Str.Get("dkp_lock_wait"));
            for (int i = 0; i < 120 && PacmanRunning(); i++) { ct.ThrowIfCancellationRequested(); Thread.Sleep(1000); }
            if (PacmanRunning()) { Warn(Str.Get("dkp_lock_busy")); return; }
        }

        // pacman's root is the folder above usr\bin (msys2 layout) or above bin.
        var binDir = Path.GetDirectoryName(pacman) ?? DevkitRoot;
        var root   = Directory.GetParent(binDir)?.FullName ?? DevkitRoot;
        if (Path.GetFileName(root).Equals("usr", StringComparison.OrdinalIgnoreCase))
            root = Directory.GetParent(root)?.FullName ?? root;

        var locks = new[]
        {
            Path.Combine(root, "var", "lib", "pacman", "db.lck"),
            Path.Combine(DevkitRoot, "msys2", "var", "lib", "pacman", "db.lck"),
        };
        foreach (var lck in locks.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (!File.Exists(lck)) continue;
            // MSYS2 creates it read-only, and File.Delete refuses read-only files.
            try { File.SetAttributes(lck, FileAttributes.Normal); File.Delete(lck); Ok(Str.Get("dkp_lock_gone", lck)); }
            catch (Exception e) { Warn(e.Message); }
        }
    }

    // make_sd.native_shell's environment for devkitPro's bash, so the checks see what the build will see
    static Dictionary<string, string> MsysEnv() => new()
    {
        ["DEVKITPRO"] = "/opt/devkitpro",
        ["CHERE_INVOKING"] = "1",
        ["MSYS2_PATH_TYPE"] = "inherit",
    };

    static void SetDevkitEnv()
    {
        var path = Environment.GetEnvironmentVariable("PATH") ?? "";
        Environment.SetEnvironmentVariable("DEVKITPRO", DevkitRoot);
        Environment.SetEnvironmentVariable("DEVKITARM", Path.Combine(DevkitRoot, "devkitARM"));
        Environment.SetEnvironmentVariable("DEVKITPPC", Path.Combine(DevkitRoot, "devkitPPC"));
        Environment.SetEnvironmentVariable("PATH", $@"{DevkitRoot}\tools\bin;{DevkitRoot}\devkitARM\bin;" + path);
    }

    // The forwarder script fetches nx-hbloader and hacBrewPack with git. It is optional: if Git cannot be
    // installed, the game still builds, without the forwarder.
    void ForwarderGit()
    {
        if (Capture("git", "--version").code == 0) return;
        Say(Str.Get("fwd_git"));
        Exec("winget", "install --id Git.Git --source winget --accept-package-agreements --accept-source-agreements --silent", check: false);
        RefreshPath();
        var gitCmd = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Git", "cmd");
        if (Capture("git", "--version").code != 0 && Directory.Exists(gitCmd))
            Environment.SetEnvironmentVariable("PATH", gitCmd + ";" + Environment.GetEnvironmentVariable("PATH"));
        if (Capture("git", "--version").code != 0)
        {
            Warn(Str.Get("fwd_git_fail"));
            ForwarderKeys = null;
        }
    }

    static void RefreshPath()
    {
        var machine = Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.Machine) ?? "";
        var user    = Environment.GetEnvironmentVariable("PATH", EnvironmentVariableTarget.User) ?? "";
        Environment.SetEnvironmentVariable("PATH", machine + ";" + user);
    }

    // ── Release ──────────────────────────────────────────────────────────────
    void Release()
    {
        Say(Str.Get("rel_checking"));
        releaseVer = LatestRelease();
        if (releaseVer != "") Ok(Str.Get("rel_found", releaseVer));
        else
        {
            // offline or GitHub unreachable: the newest release an earlier run downloaded
            releaseVer = Directory.GetFileSystemEntries(WorkDir, "SwitchWakerHD-v*")
                .Select(p => Regex.Match(Path.GetFileName(p), @"^SwitchWakerHD-(v(\d+)\.(\d+)\.(\d+))(\.zip)?$"))
                .Where(m => m.Success)
                .OrderByDescending(m => new Version(int.Parse(m.Groups[2].Value), int.Parse(m.Groups[3].Value), int.Parse(m.Groups[4].Value)))
                .Select(m => m.Groups[1].Value).FirstOrDefault() ?? "";
            if (releaseVer == "") throw new InstallException(Str.Get("rel_unknown"));
            Warn(Str.Get("rel_fallback", releaseVer));
        }

        var zip = Path.Combine(WorkDir, $"SwitchWakerHD-{releaseVer}.zip");
        ReleaseDir = Path.Combine(WorkDir, $"SwitchWakerHD-{releaseVer}");
        var baseUrl = $"https://github.com/centollOS/SwitchWakerHD/releases/download/{releaseVer}/SwitchWakerHD-{releaseVer}.zip";

        if (!File.Exists(zip))
        {
            Say(Str.Get("rel_downloading", releaseVer));
            var part = zip + ".part";
            Download(baseUrl, part);
            Say(Str.Get("rel_verifying"));
            var shaFile = zip + ".sha256";
            Download(baseUrl + ".sha256", shaFile);
            VerifySha256(part, File.ReadAllText(shaFile).Split(' ')[0].Trim());
            File.Move(part, zip);
        }
        else Ok(Str.Get("rel_already", releaseVer));

        if (!Directory.Exists(ReleaseDir))
        {
            Say(Str.Get("rel_extracting"));
            Progress?.Invoke(null);
            var tmp = ReleaseDir + ".tmp";
            if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
            try { ZipFile.ExtractToDirectory(zip, tmp); }
            catch (InvalidDataException e)
            {
                // checked when it was downloaded, so damaged on disk since: download it again next time
                Log?.Invoke(e.Message, LogKind.Error);
                File.Delete(zip);
                throw new InstallException(Str.Get("rel_corrupt"));
            }
            Directory.Move(tmp, ReleaseDir);
        }

        // Release zips come from `git archive --prefix=SwitchWakerHD-<ver>/`, so the source sits one folder down.
        if (!File.Exists(MakeSd(ReleaseDir)))
        {
            var nested = Directory.GetDirectories(ReleaseDir).FirstOrDefault(d => File.Exists(MakeSd(d)));
            if (nested == null) throw new InstallException(Str.Get("rel_no_script"));
            ReleaseDir = nested;
        }
    }

    // The latest release's tag: GitHub's API, else where github.com/.../releases/latest redirects to (the API
    // allows 60 calls an hour per address). "" when neither answers.
    string LatestRelease()
    {
        try
        {
            var json = Http.GetStringAsync("https://api.github.com/repos/centollOS/SwitchWakerHD/releases/latest").GetAwaiter().GetResult();
            var m = Regex.Match(json, "\"tag_name\":\\s*\"([^\"]+)\"");
            if (m.Success) return m.Groups[1].Value;
        }
        catch (Exception e) when (e is HttpRequestException || (e is OperationCanceledException && !ct.IsCancellationRequested))
        {
            Log?.Invoke(e.Message, LogKind.Output);
        }
        try
        {
            using var resp = Http.GetAsync("https://github.com/centollOS/SwitchWakerHD/releases/latest", HttpCompletionOption.ResponseHeadersRead, ct)
                                 .GetAwaiter().GetResult();
            var m = Regex.Match(resp.RequestMessage?.RequestUri?.AbsolutePath ?? "", @"/releases/tag/([^/]+)$");
            if (resp.IsSuccessStatusCode && m.Success) return Uri.UnescapeDataString(m.Groups[1].Value);
        }
        catch (Exception e) when (e is HttpRequestException || (e is OperationCanceledException && !ct.IsCancellationRequested))
        {
            Log?.Invoke(e.Message, LogKind.Output);
        }
        return "";
    }

    static string MakeSd(string dir) => Path.Combine(dir, "tools", "switch", "make_sd.py");

    // ── Build ────────────────────────────────────────────────────────────────
    // make_sd.py's stages ("[3/5] ...") and the compiler's own progress ("[ 45%] Building C object ..." from
    // CMake's makefiles, "[120/812] ..." from Ninja in a container) drive the bar. Each stage owns a share
    // of it, by how long it takes; compiling is most of the wait.
    static readonly double[] StageStart = { 0.00, 0.08, 0.09, 0.15, 0.95 };
    static readonly double[] StageSpan  = { 0.08, 0.01, 0.06, 0.80, 0.05 };
    static readonly Regex StageLine = new(@"^\[([1-5])/5\] ");
    static readonly Regex MakePct   = new(@"^\[\s*(\d{1,3})%\]");
    static readonly Regex NinjaStep = new(@"^\[(\d+)/(\d+)\] ");

    void Build()
    {
        Say(Str.Get("build_running"));
        Progress?.Invoke(null);
        int stage = 0, shownPct = -1;
        double best = -1;
        void Report(double p)
        {
            if (p <= best) return;   // parallel jobs can print out of order; the bar only moves forward
            best = p;
            Progress?.Invoke(p);
        }
        void OnLine(string line)
        {
            if (line.StartsWith("[+] HOME-screen forwarder")) { Status?.Invoke(Str.Get("fwd_building")); return; }
            if (line.StartsWith("  " + MinimapMod + ": ")) { Status?.Invoke(Str.Get("mm_building")); return; }
            var m = StageLine.Match(line);
            if (m.Success)
            {
                stage = int.Parse(m.Groups[1].Value);
                Report(StageStart[stage - 1]);
                Status?.Invoke(Str.Get("build_s" + stage));
                return;
            }
            if (stage != 4) return;
            double frac;
            if ((m = MakePct.Match(line)).Success) frac = int.Parse(m.Groups[1].Value) / 100.0;
            else if ((m = NinjaStep.Match(line)).Success && int.Parse(m.Groups[2].Value) > 0)
                frac = (double)int.Parse(m.Groups[1].Value) / int.Parse(m.Groups[2].Value);
            else return;
            Report(StageStart[3] + StageSpan[3] * Math.Min(frac, 1));
            var pct = (int)(frac * 100);
            if (pct > shownPct) { shownPct = pct; Status?.Invoke(Str.Get("build_s4_pct", pct)); }
        }
        // make_sd's builds use half the CPU threads, each compile ~1.5 GB: fewer on a PC without the memory for that
        // (8 threads with 8 GB of memory would need ~6 GB for the compiles alone). WWHD_JOBS reaches build.sh through make_sd's environment.
        var env = new Dictionary<string, string>();
        var ramGb = TotalRamGb();
        var jobs = Math.Max(1, Math.Min(Environment.ProcessorCount / 2, (int)((ramGb - 2) / 1.5)));
        if (ramGb > 0 && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("WWHD_JOBS")))
        {
            env["WWHD_JOBS"] = jobs.ToString();
            Info(Str.Get("build_jobs", jobs, Environment.ProcessorCount, ramGb.ToString("0")));
        }
        var tail = new Queue<string>();
        var forwarder = ForwarderKeys != null ? " --keys " + Quote(ForwarderKeys) : " --no-forwarder";
        var minimap = MinimapGame != null ? $" --guest-mod {MinimapMod} --gc-game {Quote(MinimapGame)}" : "";
        var rc = Exec(pythonCmd, $"{Quote(MakeSd(ReleaseDir))} {GameArg} {Quote(GamePath)}{forwarder}{minimap}", ReleaseDir, check: false,
                      onLine: line => { OnLine(line); tail.Enqueue(line); if (tail.Count > 300) tail.Dequeue(); }, env: env);
        Progress?.Invoke(rc == 0 ? 1 : (double?)null);
        if (rc != 0)
        {
            // An object file cut short by an interrupted build (newer than its source, so make keeps it) or a CMake
            // cache from another folder fails every retry the same way: start the Switch build over.
            var broken = new Regex(@"file truncated|file format not recognized|error adding symbols|CMakeCache\.txt directory .* is different", RegexOptions.IgnoreCase);
            var buildDir = Path.Combine(ReleaseDir, "build", "switch-dk");
            if (tail.Any(broken.IsMatch) && Directory.Exists(buildDir))
            {
                try { Directory.Delete(buildDir, true); Warn(Str.Get("build_reset", buildDir)); }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { Warn(e.Message); }
            }
            throw new InstallException(ExplainBuildFailure(rc, tail.ToList()));
        }
        Ok(SdOut);
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct MEMORYSTATUSEX
    {
        public uint dwLength, dwMemoryLoad;
        public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX status);

    // The PC's memory in GB, or 0 when Windows does not say.
    static double TotalRamGb()
    {
        var st = new MEMORYSTATUSEX { dwLength = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(MEMORYSTATUSEX)) };
        try { return GlobalMemoryStatusEx(ref st) ? st.ullTotalPhys / 1073741824.0 : 0; }
        catch (Exception) { return 0; }
    }

    // The build's last lines, turned into what the error box shows: make_sd's own reason (its "make_sd: ..." line),
    // the last error line before it, and a hint for the failures players have met.
    internal static string ExplainBuildFailure(int rc, IList<string> tail)
    {
        var lines = tail.Select(l => l.Trim()).Where(l => l != "").ToList();
        var msg = new StringBuilder(Str.Get("build_fail", rc));
        var why = lines.FindLastIndex(l => l.StartsWith("make_sd: "));
        if (why >= 0) msg.Append("\n").Append(lines[why]);
        // the code mods' step (tools/switch/guest_mods.py) says what stopped it on its own line
        var mods = lines.FindLastIndex(l => l.StartsWith("guest_mods: "));
        if (mods >= 0) msg.Append("\n").Append(lines[mods]);
        // a compiler's first "error:" says more than make's "*** Error 2" after it
        var before = lines.Take(why >= 0 ? why : lines.Count).Where(l => !l.StartsWith("$ ")).ToList();
        var errLine = before.FirstOrDefault(l => Regex.IsMatch(l, @"\berror\s*:", RegexOptions.IgnoreCase))
                   ?? before.LastOrDefault(l => Regex.IsMatch(l, @"\b(error|fatal|not found|denied)\b", RegexOptions.IgnoreCase));
        if (errLine != null) msg.Append("\n").Append(errLine.Length > 300 ? errLine.Substring(0, 300) + "..." : errLine);

        var all = string.Join("\n", lines);
        bool Has(string pattern) => Regex.IsMatch(all, pattern, RegexOptions.IgnoreCase);
        string? hint =
            mods >= 0                                                           ? "hint_minimap" :
            Has(@"disc key (was )?not found")                                   ? "hint_disc_key" :
            Has(@"common key not found")                                        ? "hint_common_key" :
            Has(@"bad FST magic|wrong key|not this disc's key")                 ? "hint_wrong_key" :
            Has(@"No space left on device|not enough space on the disk|ENOSPC") ? "hint_disk" :
            Has(@"out of memory|virtual memory exhausted|MemoryError|Cannot allocate memory") ? "hint_memory" :
            Has(@"cannot specify '-o' with '-c'")                               ? "hint_path_space" :
            Has(@"file truncated|file format not recognized|error adding symbols|CMakeCache\.txt directory .* is different") ? "hint_broken_obj" :
            Has(@"command not found")                                           ? "hint_tool_missing" :
            Has(@"Permission denied|Access is denied|PermissionError|Device or resource busy|Operation not permitted") ? "hint_av" :
            null;
        // #28: build.sh stopped and printed nothing at all (a tool that cannot start, such as an MSYS2 program
        // whose library is missing after a partial update, or one blocked by an antivirus)
        if (hint == null && why > 0 && lines[why].Contains("this step failed") &&
            (lines[why - 1].StartsWith("$ ") || Regex.IsMatch(lines[why - 1], @"^build\.sh: line \d+: .* failed \(exit code \d+\)$")))
            hint = "hint_silent";
        if (hint != null) msg.Append("\n\n").Append(Str.Get(hint));
        return msg.ToString();
    }

    // ── SD card (from the done page) ─────────────────────────────────────────
    public void CopyToSd(string drive, CancellationToken token)
    {
        ct = token;
        Say(Str.Get("sd_copying", drive));
        Progress?.Invoke(null);

        // Room for what is new or bigger (files already on the card are replaced), and FAT32's 4 GB file limit.
        var di = new DriveInfo(drive);
        long need = 0, biggest = 0;
        foreach (var f in Directory.GetFiles(SdOut, "*", SearchOption.AllDirectories))
        {
            var len = new FileInfo(f).Length;
            var dst = new FileInfo(Path.Combine(drive, f.Substring(SdOut.Length).TrimStart('\\')));
            need += Math.Max(0, len - (dst.Exists ? dst.Length : 0));
            biggest = Math.Max(biggest, len);
        }
        if (di.AvailableFreeSpace < need)
            throw new InstallException(Str.Get("sd_no_space", drive, (di.AvailableFreeSpace / 1e9).ToString("0.0"), (need / 1e9).ToString("0.0")));
        if (biggest >= 4L << 30 && di.DriveFormat.Equals("FAT32", StringComparison.OrdinalIgnoreCase))
            throw new InstallException(Str.Get("sd_fat32", drive));
        if (!Directory.Exists(Path.Combine(drive, "atmosphere"))) Warn(Str.Get("sd_no_atmo", drive));
        // robocopy: 0-7 is success (files copied, extra files, ...), 8 and up is failure.
        var rc = Exec("robocopy", $"{Quote(SdOut)} {Quote(drive)} /E /R:2 /W:2 /NP /NDL /NJH /NJS", check: false);
        if (rc < 0 || rc >= 8) throw new InstallException(Str.Get("sd_fail", rc));
        Ok(Str.Get("sd_copied", drive));
    }

    // ── Helpers ──────────────────────────────────────────────────────────────
    void Download(string url, string dest)
    {
        Log?.Invoke(url, LogKind.Info);
        using var response = Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct).GetAwaiter().GetResult();
        if (!response.IsSuccessStatusCode) throw new InstallException(Str.Get("err_download", url));

        var total = response.Content.Headers.ContentLength ?? -1L;
        using var src  = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();
        using var file = File.Create(dest);
        var buf = new byte[81920];
        long done = 0; int read; var last = DateTime.MinValue;
        while ((read = src.Read(buf, 0, buf.Length)) > 0)
        {
            ct.ThrowIfCancellationRequested();
            file.Write(buf, 0, read);
            done += read;
            if (total > 0 && (DateTime.UtcNow - last).TotalMilliseconds > 150)
            {
                last = DateTime.UtcNow;
                Progress?.Invoke((double)done / total);
                Status?.Invoke(Str.Get("download_prog", (done / 1048576.0).ToString("0.0"), (total / 1048576.0).ToString("0.0")));
            }
        }
        Progress?.Invoke(null);
    }

    void VerifySha256(string file, string expected)
    {
        string hash;
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(file))
            hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
        if (!string.Equals(hash, expected, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(file);
            throw new InstallException(Str.Get("err_integrity"));
        }
        Ok("SHA256 " + hash.ToLowerInvariant());
    }

    // Runs a process, streams its output to the log; with check, a non-zero exit is a failure.
    int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null,
             IDictionary<string, string>? env = null)
    {
        Log?.Invoke($"> {exe} {args}", LogKind.Info);
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute        = false,
            CreateNoWindow         = true,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding  = Encoding.UTF8,
            WorkingDirectory       = workDir ?? WorkDir,
        };
        psi.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
        psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
        if (env != null) foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;

        int rc;
        using var job = new ProcessJob(killOnClose: true);
        var gate = new object();   // stdout and stderr arrive on two threads; onLine sees one line at a time
        try
        {
            using var proc = new Process { StartInfo = psi };
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) lock (gate) { Log?.Invoke(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.ErrorDataReceived  += (_, e) => { if (e.Data != null) lock (gate) { Log?.Invoke(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.Start();
            job.Add(proc);
            currentJob = job;
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();
            while (!proc.WaitForExit(250))
            {
                if (ct.IsCancellationRequested) { job.Terminate(); ct.ThrowIfCancellationRequested(); }
            }
            // Anything it left behind (gpg-agent after pacman, ...) would hold the output pipes open and
            // make the wait below hang.
            job.Terminate();
            // Drains the redirected output; bounded, in case a process got out of the job and keeps the pipe.
            var drained = new ManualResetEventSlim();
            ThreadPool.QueueUserWorkItem(_ => { try { proc.WaitForExit(); } catch { } drained.Set(); });
            drained.Wait(TimeSpan.FromSeconds(5));
            ct.ThrowIfCancellationRequested();   // killed by KillCurrent: a cancel, not a failure
            rc = proc.ExitCode;
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception e)
        {
            Log?.Invoke(e.Message, LogKind.Error);
            rc = -1;
        }
        finally { currentJob = null; }

        if (check && rc != 0) throw new InstallException(Str.Get("cmd_fail", Path.GetFileName(exe), rc));
        return rc;
    }

    static (int code, string output) Capture(string exe, string args)
    {
        try
        {
            var psi = new ProcessStartInfo(exe, args)
            {
                UseShellExecute        = false,
                CreateNoWindow         = true,
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

    public void KillCurrent() => currentJob?.Terminate();

    void Say(string msg)  { Status?.Invoke(msg); Log?.Invoke(msg, LogKind.Info); }
    void Info(string msg) => Log?.Invoke(msg, LogKind.Info);
    void Ok(string msg)   => Log?.Invoke(msg, LogKind.Ok);
    void Warn(string msg) => Log?.Invoke(msg, LogKind.Warn);
}
