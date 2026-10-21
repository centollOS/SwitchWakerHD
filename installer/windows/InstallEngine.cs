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

sealed class InstallEngine : EngineBase
{
    protected override string Work => WorkDir;

    // The player's pick on the welcome page, remembered from the last run (BuildFolder), or ChooseWorkDir's
    public static string WorkDir = BuildFolder.Load(ChooseWorkDir);

    // %USERPROFILE%\SwitchWakerHD-Build, unless the user name has a space or a non-ASCII letter ("JAM Visuals",
    // issue #30): the build tools do not all cope with those, so then a folder at the root of the system drive
    // (C:\SwitchWakerHD-Build), which standard users may create. Back to the profile if that cannot be created.
    // Also there when the profile path is long: the release is unpacked two folders down and the deepest object
    // file adds ~160 characters, and devkitPro's gcc cannot open paths over 260 (MAX_PATH).
    const int MaxWorkDirLength = BuildFolder.MaxWindowsLength;
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

    string pacman     = "";
    ProcessJob? currentJob;   // the running process and all it started; KillCurrent stops them

    // ── Python ───────────────────────────────────────────────────────────────
    protected override void Python()
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
    protected override void Toolchain() => DevkitPro();

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
            OnProgress(null);
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
        if (reason == "dkp_why_403")
        {
            Warn(Str.Get("dkp_403_xfer"));
            ClearStalePacmanLock();
            output.Clear();
            if (PacmanThroughPython("-Sy --needed --noconfirm " + packages, output) == 0) reason = null;
            else reason = PacmanFailure(output);
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

    // pacman again with its downloads through the installer's Python (EngineBase.WritePacmanFetch): a copy of
    // its pacman.conf with an XferCommand. pacman is an MSYS2 program: the Python and the script are given to it
    // as /c/... paths; MSYS2 turns its POSIX arguments (%o) into Windows ones for a Windows Python, and the
    // script joins them to MSYS2's folder if not.
    int PacmanThroughPython(string args, List<string> output)
    {
        var root = PacmanRoot();
        var conf = Path.Combine(root, @"etc\pacman.conf");
        var copy = Path.Combine(root, @"tmp\wwhd-xfer.conf");
        var python = Capture(pythonCmd, "-c \"import sys;print(sys.executable)\"").output.Trim();
        if (!File.Exists(conf) || !File.Exists(python)) return -1;
        var xfer = $"XferCommand = \"{MsysPath(python)}\" \"{MsysPath(WritePacmanFetch())}\" \"{root.Replace('\\', '/')}\" %u %o";
        var lines = new List<string>();
        foreach (var line in File.ReadAllLines(conf))
        {
            if (line.TrimStart().StartsWith("XferCommand", StringComparison.OrdinalIgnoreCase)) continue;
            lines.Add(line);
            if (line.Trim().Equals("[options]", StringComparison.OrdinalIgnoreCase)) lines.Add(xfer);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
        File.WriteAllLines(copy, lines);
        try { return Exec(pacman, $"--config \"{copy.Replace('\\', '/')}\" " + args, check: false, onLine: output.Add); }
        finally { try { File.Delete(copy); } catch (IOException) { } }
    }

    // C:\Users\x -> /c/Users/x
    static string MsysPath(string windows) =>
        windows.Length > 2 && windows[1] == ':' ? "/" + char.ToLowerInvariant(windows[0]) + windows.Substring(2).Replace('\\', '/')
                                                : windows.Replace('\\', '/');

    // pacman's root: the folder above usr\bin (msys2 layout) or above bin
    string PacmanRoot()
    {
        var binDir = Path.GetDirectoryName(pacman) ?? DevkitRoot;
        var root   = Directory.GetParent(binDir)?.FullName ?? DevkitRoot;
        if (Path.GetFileName(root).Equals("usr", StringComparison.OrdinalIgnoreCase))
            root = Directory.GetParent(root)?.FullName ?? root;
        return root;
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

        var root = PacmanRoot();
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


    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct MEMORYSTATUSEX
    {
        public uint dwLength, dwMemoryLoad;
        public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX status);

    // The PC's memory in GB, or 0 when Windows does not say.
    protected override double TotalRamGb()
    {
        var st = new MEMORYSTATUSEX { dwLength = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(MEMORYSTATUSEX)) };
        try { return GlobalMemoryStatusEx(ref st) ? st.ullTotalPhys / 1073741824.0 : 0; }
        catch (Exception) { return 0; }
    }

    // ── SD card (from the done page) ─────────────────────────────────────────
    // progress(done bytes, total bytes): the check after the copy (robocopy shows none)
    public void CopyToSd(string drive, Action<long, long> progress, CancellationToken token)
    {
        ct = token;
        Say(Str.Get("sd_copying", drive));
        OnProgress(null);

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
        // robocopy skips a file with the same size and date: every file read back from the card
        VerifyCard(drive, Directory.GetFiles(SdOut, "*", SearchOption.AllDirectories), f => new UncachedFile(f),
                   (src, dst) =>
                   {
                       Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                       if (File.Exists(dst)) File.SetAttributes(dst, FileAttributes.Normal);
                       File.Copy(src, dst, true);
                   },
                   progress);
        Ok(Str.Get("sd_copied", drive));
    }

    // Runs a process, streams its output to the log; with check, a non-zero exit is a failure.
    protected override int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null,
             IDictionary<string, string>? env = null)
    {
        OnLog($"> {exe} {args}", LogKind.Info);
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
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) lock (gate) { OnLog(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.ErrorDataReceived  += (_, e) => { if (e.Data != null) lock (gate) { OnLog(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
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
            OnLog(e.Message, LogKind.Error);
            rc = -1;
        }
        finally { currentJob = null; }

        if (check && rc != 0) throw new InstallException(Str.Get("cmd_fail", Path.GetFileName(exe), rc));
        return rc;
    }

    public override void KillCurrent() => currentJob?.Terminate();
}
