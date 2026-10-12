// The install steps, run off the UI thread: output goes to events, and failures throw InstallException.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
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
    public static readonly string WorkDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "SwitchWakerHD-Build");
    const string DevkitRoot = @"C:\devkitPro";

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

    public string ReleaseDir { get; private set; } = "";
    public string SdOut => Path.Combine(ReleaseDir, "build", "sd");

    string pythonCmd  = "python";
    string pacman     = "";
    string releaseVer = "v0.4.3";
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
        catch { StepChanged?.Invoke(step, StepState.Failed); throw; }
        StepChanged?.Invoke(step, StepState.Done);
    }

    // ── Python ───────────────────────────────────────────────────────────────
    void Python()
    {
        if (!FindPython())
        {
            Say(Str.Get("py_installing"));
            // PrependPath: winget's own silent switches leave python.exe off PATH.
            Exec("winget", "install --id Python.Python.3.12 --source winget --accept-package-agreements " +
                           "--accept-source-agreements --silent --override \"/quiet InstallAllUsers=1 PrependPath=1 Include_launcher=1\"");
            RefreshPath();
            if (!FindPython()) throw new InstallException(Str.Get("py_fail"));
        }
        Say(Str.Get("py_pkgs"));
        Exec(pythonCmd, "-m pip install --disable-pip-version-check zstandard pycryptodome" + (ForwarderKeys != null ? " pillow" : ""));
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
        foreach (var cmd in candidates)
        {
            var (code, output) = Capture(cmd, "--version");
            if (code == 0 && output.Contains("Python 3."))
            {
                pythonCmd = cmd;
                Ok(Str.Get("py_found", output.Trim()));
                return true;
            }
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
        // gcc (MSYS2's, for the PC): the forwarder script compiles hacBrewPack, the NSP packer, with it
        var packages = "switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake cmake make" + (ForwarderKeys != null ? " gcc" : "");
        if (Exec(pacman, "-Sy --needed --noconfirm " + packages, check: false) != 0)
        {
            // pkg.devkitpro.org often answers 403 (Cloudflare, devkitPro/pacman#63) and the whole -Sy gives up
            // without installing anything. The package lists it did refresh, and the ones from earlier runs,
            // are still on disk: install from those.
            Warn(Str.Get("dkp_pkg_warn"));
            ClearStalePacmanLock();
            Exec(pacman, "-S --needed --noconfirm " + packages, check: false);
        }
        CheckDevkitTools();
    }

    // What the build needs, by the files it runs or links. A missing one stops here, with the reason,
    // instead of after the download and two build stages (issue #28).
    void CheckDevkitTools()
    {
        var needed = new (string name, string file)[]
        {
            ("cmake",       @"msys2\usr\bin\cmake.exe"),
            ("make",        @"msys2\usr\bin\make.exe"),
            ("devkitA64",   @"devkitA64\bin\aarch64-none-elf-gcc.exe"),
            ("libnx",       @"libnx\lib\libnx.a"),
            ("switch-cmake", @"cmake\Switch.cmake"),
            ("switch-zlib", @"portlibs\switch\lib\libz.a"),
            ("switch-lz4",  @"portlibs\switch\lib\liblz4.a"),
        };
        var missing = needed.Where(t => !File.Exists(Path.Combine(DevkitRoot, t.file))).Select(t => t.name).ToList();
        if (missing.Count > 0) throw new InstallException(Str.Get("dkp_missing", string.Join(", ", missing)));

        if (ForwarderKeys != null && !File.Exists(Path.Combine(DevkitRoot, @"msys2\usr\bin\gcc.exe")))
        {
            Warn(Str.Get("fwd_gcc_fail"));
            ForwarderKeys = null;
        }
        Ok(Str.Get("dkp_tools_ok"));
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
        try
        {
            var json = Http.GetStringAsync("https://api.github.com/repos/centollOS/SwitchWakerHD/releases/latest").GetAwaiter().GetResult();
            var m = Regex.Match(json, "\"tag_name\":\\s*\"([^\"]+)\"");
            if (m.Success) releaseVer = m.Groups[1].Value;
            Ok(Str.Get("rel_found", releaseVer));
        }
        catch { Warn(Str.Get("rel_fallback", releaseVer)); }

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
            ZipFile.ExtractToDirectory(zip, tmp);
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
        var forwarder = ForwarderKeys != null ? $" --keys \"{ForwarderKeys}\"" : " --no-forwarder";
        var rc = Exec(pythonCmd, $"\"{MakeSd(ReleaseDir)}\" {GameArg} \"{GamePath}\"{forwarder}", ReleaseDir, check: false, onLine: OnLine);
        Progress?.Invoke(rc == 0 ? 1 : (double?)null);
        if (rc != 0) throw new InstallException(Str.Get("build_fail", rc));
        Ok(SdOut);
    }

    // ── SD card (from the done page) ─────────────────────────────────────────
    public void CopyToSd(string drive, CancellationToken token)
    {
        ct = token;
        Say(Str.Get("sd_copying", drive));
        Progress?.Invoke(null);
        // robocopy: 0-7 is success (files copied, extra files, ...), 8 and up is failure.
        // "D:\" would escape the closing quote (Windows argv rules); "D:\\" is D:\ for robocopy.
        var rc = Exec("robocopy", $"\"{SdOut}\" \"{drive.TrimEnd('\\')}\\\\\" /E /R:2 /W:2 /NP /NDL /NJH /NJS", check: false);
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
    int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null)
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

        int rc;
        using var job = new ProcessJob(killOnClose: true);
        try
        {
            using var proc = new Process { StartInfo = psi };
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) { Log?.Invoke(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.ErrorDataReceived  += (_, e) => { if (e.Data != null) { Log?.Invoke(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
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

        if (check && rc != 0) throw new InstallException($"{Path.GetFileName(exe)} {rc}");
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
