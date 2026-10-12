// The Windows steps (shared/EngineBase.cs has the rest): Python through winget, devkitPro through its own
// installer and MSYS2's pacman, processes in job objects, robocopy for the SD card.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;

namespace SwitchWakerHD.Installer;

sealed class InstallEngine : EngineBase
{
    const string DevkitRoot = @"C:\devkitPro";

    string pacman = "";
    ProcessJob? currentJob;   // the running process and all it started; KillCurrent stops them

    // ── Python ───────────────────────────────────────────────────────────────
    protected override void Python()
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
    protected override void Toolchain()
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

            Ask(Str.Get("dkp_notice_h"), Str.Get("dkp_notice"), Str.Get("dkp_open"));
            Say(Str.Get("dkp_wait"));
            SetProgress(null);
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
                Ask(Str.Get("dkp_manual_h"), Str.Get("dkp_manual"), Str.Get("btn_continue"));
                pacman = FindPacman();
                if (pacman == "") throw new InstallException(Str.Get("dkp_not_found"));
            }
        }
        Ok(Str.Get("dkp_found_at", pacman));

        if (ForwarderKeys != null) ForwarderGit();

        Say(Str.Get("dkp_installing"));
        SetDevkitEnv();
        ClearStalePacmanLock();
        // gcc (MSYS2's, for the PC): the forwarder script compiles hacBrewPack, the NSP packer, with it
        var packages = "switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake" + (ForwarderKeys != null ? " gcc" : "");
        if (Exec(pacman, "-Sy --needed --noconfirm " + packages, check: false) != 0)
            Warn(Str.Get("dkp_pkg_warn"));
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

    // ── SD card (from the done page) ─────────────────────────────────────────
    public void CopyToSd(string drive, CancellationToken token)
    {
        ct = token;
        Say(Str.Get("sd_copying", drive));
        SetProgress(null);
        // robocopy: 0-7 is success (files copied, extra files, ...), 8 and up is failure.
        // "D:\" would escape the closing quote (Windows argv rules); "D:\\" is D:\ for robocopy.
        var rc = Exec("robocopy", $"\"{SdOut}\" \"{drive.TrimEnd('\\')}\\\\\" /E /R:2 /W:2 /NP /NDL /NJH /NJS", check: false);
        if (rc < 0 || rc >= 8) throw new InstallException(Str.Get("sd_fail", rc));
        Ok(Str.Get("sd_copied", drive));
    }

    // ── Processes ────────────────────────────────────────────────────────────
    protected override int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null)
    {
        LogLine($"> {exe} {args}", LogKind.Info);
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
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) { LogLine(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.ErrorDataReceived  += (_, e) => { if (e.Data != null) { LogLine(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
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
            LogLine(e.Message, LogKind.Error);
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

    public override void KillCurrent() => currentJob?.Terminate();
}
