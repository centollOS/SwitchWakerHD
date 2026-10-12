// The Linux steps (shared/EngineBase.cs has the rest): Python in its own venv under the working folder (no
// root, no pip on the system), and the Switch toolchain: an installed devkitPro, else a usable Docker or
// Podman (make_sd.py then builds in the devkitpro/devkita64 image), else it installs one, with the user's
// password: devkitPro on Debian/Ubuntu (its official apt route), Podman elsewhere. Root steps go through
// pkexec when a desktop can ask for the password, else the user runs one command in a terminal.
// Processes run in their own process group (setsid), so a cancel kills the whole build at once.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace SwitchWakerHD.Installer;

sealed class LinuxEngine : EngineBase
{
    const string DkpRoot = "/opt/devkitpro";
    const string DkpImage = "docker.io/devkitpro/devkita64:latest";
    static readonly string[] DkpPackages = { "switch-dev", "deko3d", "uam", "switch-lz4", "switch-zlib", "switch-cmake" };

    string? containerEngine;         // docker or podman, when the build runs in a container
    int currentGroup;                // process group of the running process (its pid, thanks to setsid)
    readonly object groupLock = new();

    // ── Python ───────────────────────────────────────────────────────────────
    protected override void Python()
    {
        var python3 = FindOnPath("python3") ?? FindOnPath("python");
        if (python3 == null || !Capture(python3, "--version").output.Contains("Python 3."))
        {
            Warn(Str.Get("lx_py_missing"));
            RunAsRoot(Str.Get("lx_tc_what_python"), PackageScript(new Dictionary<string, string>
            {
                ["apt-get"] = "apt-get update && apt-get install -y python3",
                ["dnf"] = "dnf install -y python3",
                ["pacman"] = "pacman -S --needed --noconfirm python",
                ["zypper"] = "zypper --non-interactive install python3",
            }));
            python3 = FindOnPath("python3") ?? throw new InstallException(Str.Get("lx_py_missing"));
        }
        Ok(Str.Get("py_found", Capture(python3, "--version").output.Trim()));

        // A venv of our own: distributions forbid pip on the system Python (PEP 668), and Debian/Ubuntu
        // split venv's pip out (python3-venv). --without-pip plus get-pip.py needs neither root nor that package.
        var venv = Path.Combine(WorkDir, "venv");
        var vpy = Path.Combine(venv, "bin", "python");
        if (!File.Exists(vpy) || Capture(vpy, "--version").code != 0)
        {
            Say(Str.Get("lx_py_venv"));
            if (Directory.Exists(venv)) Directory.Delete(venv, true);
            if (Exec(python3, $"-m venv \"{venv}\"", check: false) != 0)
            {
                if (Directory.Exists(venv)) Directory.Delete(venv, true);
                Exec(python3, $"-m venv --without-pip \"{venv}\"");
            }
        }
        if (Capture(vpy, "-m pip --version").code != 0)
        {
            Say(Str.Get("lx_py_getpip"));
            var getPip = Path.Combine(WorkDir, "get-pip.py");
            Download("https://bootstrap.pypa.io/get-pip.py", getPip);
            Exec(vpy, $"\"{getPip}\" --disable-pip-version-check --no-warn-script-location");
        }
        Say(Str.Get("py_pkgs"));
        Exec(vpy, "-m pip install --disable-pip-version-check zstandard pycryptodome");
        pythonCmd = vpy;
    }

    // ── Switch toolchain ─────────────────────────────────────────────────────
    protected override void Toolchain()
    {
        Say(Str.Get("dkp_searching"));
        for (int attempt = 0; ; attempt++)
        {
            if (UseNativeDevkitPro(out var missing)) return;
            if (UseContainer()) return;
            if (attempt > 0) throw new InstallException(Str.Get("lx_tc_not_found"));

            // Nothing to build with yet: install one.
            var pm = PackageManager();
            if (pm == "apt-get")
            {
                RunAsRoot(Str.Get("lx_tc_what_dkp"), DevkitProAptScript(missing));
            }
            else if (pm is "dnf" or "pacman" or "zypper")
            {
                RunAsRoot(Str.Get("lx_tc_what_podman"), PackageScript(new Dictionary<string, string>
                {
                    ["dnf"] = "dnf install -y podman",
                    ["pacman"] = "pacman -S --needed --noconfirm podman",
                    ["zypper"] = "zypper --non-interactive install podman",
                }));
            }
            else Ask(Str.Get("lx_root_h"), Str.Get("lx_tc_unknown"), Str.Get("btn_done_continue"));
        }
    }

    // devkitPro under /opt/devkitpro with every Switch package, plus the host's cmake and make: build.sh then
    // builds natively (WWHD_NATIVE=1 makes it skip any container engine).
    bool UseNativeDevkitPro(out bool onlyPackagesMissing)
    {
        onlyPackagesMissing = false;
        var pacman = FindPacman();
        if (pacman == null || !Directory.Exists(DkpRoot)) return false;
        onlyPackagesMissing = true;
        var missing = DkpPackages.Where(p => Capture(pacman, "-Q " + p).code != 0).ToList();
        if (missing.Count > 0) { Warn(Str.Get("lx_tc_pkgs", string.Join(" ", missing))); return false; }
        var hostTools = new[] { "cmake", "make" }.Where(t => FindOnPath(t) == null).ToList();
        if (hostTools.Count > 0) { Warn(Str.Get("lx_tc_hosttools", string.Join(" ", hostTools))); return false; }

        Environment.SetEnvironmentVariable("DEVKITPRO", DkpRoot);
        Environment.SetEnvironmentVariable("DEVKITA64", Path.Combine(DkpRoot, "devkitA64"));
        Environment.SetEnvironmentVariable("WWHD_NATIVE", "1");
        containerEngine = null;
        Ok(Str.Get("lx_tc_native", DkpRoot));
        return true;
    }

    static string? FindPacman() =>
        FindOnPath("dkp-pacman") ??
        new[] { "/opt/devkitpro/pacman/bin/pacman", "/usr/bin/dkp-pacman" }.FirstOrDefault(File.Exists) ??
        // Arch with devkitPro's repositories: the system pacman knows the packages
        (Capture("pacman", "-Q switch-dev").code == 0 ? "pacman" : null);

    // build.sh picks podman first, then docker, so test them in that order; installed is not enough, the
    // user must be allowed to talk to it (docker's socket wants the docker group).
    bool UseContainer()
    {
        foreach (var engine in new[] { "podman", "docker" })
        {
            if (FindOnPath(engine) == null) continue;
            var (code, output) = Capture(engine, "info", TimeSpan.FromSeconds(30));
            if (code != 0)
            {
                Warn(Str.Get("lx_tc_docker_denied", engine));
                LogLine(output.Trim(), LogKind.Output);
                continue;
            }
            containerEngine = engine;
            Environment.SetEnvironmentVariable("WWHD_NATIVE", null);
            Ok(Str.Get("lx_tc_container", engine));
            return true;
        }
        return false;
    }

    // devkitPro's own apt route (https://devkitpro.org/wiki/devkitPro_pacman): their installer adds the apt
    // repository and dkp-pacman; then the Switch packages, and the host tools build.sh runs.
    static string DevkitProAptScript(bool onlyPackages)
    {
        var sb = new StringBuilder();
        sb.AppendLine("set -e");
        sb.AppendLine("export DEBIAN_FRONTEND=noninteractive");
        sb.AppendLine("apt-get update");
        sb.AppendLine("apt-get install -y cmake make wget ca-certificates gnupg");
        if (!onlyPackages)
        {
            sb.AppendLine("cd \"$(mktemp -d)\"");
            sb.AppendLine("wget -q https://apt.devkitpro.org/install-devkitpro-pacman");
            sb.AppendLine("chmod +x ./install-devkitpro-pacman");
            sb.AppendLine("yes | ./install-devkitpro-pacman");
        }
        sb.AppendLine("dkp-pacman -Sy --needed --noconfirm " + string.Join(" ", DkpPackages));
        return sb.ToString();
    }

    static string? PackageManager() =>
        new[] { "apt-get", "dnf", "pacman", "zypper" }.FirstOrDefault(pm => FindOnPath(pm) != null);

    static string PackageScript(Dictionary<string, string> byManager)
    {
        var pm = PackageManager();
        if (pm == null || !byManager.TryGetValue(pm, out var cmd)) throw new InstallException(Str.Get("lx_tc_unknown"));
        return "set -e\n" + cmd + "\n";
    }

    // Runs a shell script as root: through pkexec when a desktop session can ask for the password, else the
    // user runs it with sudo in a terminal (the notice shows the command to paste).
    void RunAsRoot(string what, string script)
    {
        var path = Path.Combine(WorkDir, "root-step.sh");
        File.WriteAllText(path, "#!/bin/sh\n# SwitchWakerHD installer: " + what + "\n" + script);
        LogLine(script.Trim(), LogKind.Output);

        var graphical = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DISPLAY")) ||
                        !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("WAYLAND_DISPLAY"));
        if (graphical && FindOnPath("pkexec") != null)
        {
            Say(Str.Get("lx_root_pkexec", what));
            var rc = Exec("pkexec", $"/bin/sh \"{path}\"", check: false);
            if (rc == 0) return;
            // 126: the password window was dismissed; 127: no polkit agent or not authorised
            Warn(Str.Get("lx_root_failed", rc));
        }
        Ask(Str.Get("lx_root_h"), Str.Get("lx_root_body", what), Str.Get("btn_done_continue"), $"sudo sh '{path}'");
    }

    // ── SD card (from the done page) ─────────────────────────────────────────
    // Copies what is new or changed (size, or older on the card), then syncs; the rest of the card stays as is.
    public void CopyToSd(string mountPoint, Action<long, long> progress, CancellationToken token)
    {
        ct = token;
        var files = Directory.GetFiles(SdOut, "*", SearchOption.AllDirectories);
        long total = files.Sum(f => new FileInfo(f).Length), done = 0;
        var buf = new byte[1 << 20];
        foreach (var src in files)
        {
            ct.ThrowIfCancellationRequested();
            var rel = src.Substring(SdOut.Length).TrimStart('/');
            var dst = Path.Combine(mountPoint, rel);
            var si = new FileInfo(src);
            var di = new FileInfo(dst);
            if (!di.Exists || di.Length != si.Length || di.LastWriteTimeUtc < si.LastWriteTimeUtc.AddSeconds(-2))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                using (var i = File.OpenRead(src))
                using (var o = File.Create(dst))
                {
                    int n;
                    while ((n = i.Read(buf, 0, buf.Length)) > 0)
                    {
                        ct.ThrowIfCancellationRequested();
                        o.Write(buf, 0, n);
                        done += n;
                        progress(done, total);
                    }
                }
                File.SetLastWriteTimeUtc(dst, si.LastWriteTimeUtc);
            }
            else { done += si.Length; progress(done, total); }
        }
        Exec("sync", "", check: false);
        Ok(Str.Get("sd_copied_lx", mountPoint));
    }

    // ── Processes ────────────────────────────────────────────────────────────
    // setsid makes the process the leader of a new process group (its pgid is its pid), so kill(-pid) stops
    // it and every child at once: python, bash, make and the compilers.
    protected override int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null)
    {
        LogLine($"> {exe} {args}", LogKind.Info);
        var psi = new ProcessStartInfo("setsid", $"\"{exe}\" {args}")
        {
            UseShellExecute        = false,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            RedirectStandardInput  = true,   // nothing may stop to ask a question on our stdin
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding  = Encoding.UTF8,
            WorkingDirectory       = workDir ?? WorkDir,
        };
        psi.Environment["PYTHONUNBUFFERED"] = "1";
        psi.Environment["PYTHONIOENCODING"] = "utf-8";

        int rc;
        try
        {
            using var proc = new Process { StartInfo = psi };
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) { LogLine(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.ErrorDataReceived  += (_, e) => { if (e.Data != null) { LogLine(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.Start();
            proc.StandardInput.Close();
            lock (groupLock) currentGroup = proc.Id;
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();
            while (!proc.WaitForExit(250))
            {
                if (ct.IsCancellationRequested) { KillCurrent(); ct.ThrowIfCancellationRequested(); }
            }
            // Anything left in the group (gpg-agent after pacman, ...) would hold the output pipes open.
            KillGroup(proc.Id);
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
        finally { lock (groupLock) currentGroup = 0; }

        if (check && rc != 0) throw new InstallException($"{Path.GetFileName(exe)} {rc}");
        return rc;
    }

    public override void KillCurrent()
    {
        int group;
        lock (groupLock) group = currentGroup;
        if (group > 0) KillGroup(group);
        // A container runs under the engine's daemon (or conmon), not in our process group.
        if (containerEngine != null)
        {
            var (_, ids) = Capture(containerEngine, $"ps -q --filter ancestor={DkpImage}");
            foreach (var id in ids.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries))
                Capture(containerEngine, "kill " + id.Trim());
        }
    }

    [DllImport("libc", SetLastError = true)]
    static extern int kill(int pid, int sig);

    static void KillGroup(int pgid)
    {
        try { kill(-pgid, 9); } catch { }
    }

    static string? FindOnPath(string name)
    {
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':'))
        {
            if (dir.Length == 0) continue;
            var p = Path.Combine(dir, name);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    static (int code, string output) Capture(string exe, string args, TimeSpan? timeout = null)
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
            var stdout = proc.StandardOutput.ReadToEndAsync();
            var stderr = proc.StandardError.ReadToEndAsync();
            if (!proc.WaitForExit((int)(timeout ?? TimeSpan.FromSeconds(60)).TotalMilliseconds))
            {
                try { proc.Kill(true); } catch { }
                return (-1, "");
            }
            return (proc.ExitCode, stdout.Result + stderr.Result);
        }
        catch { return (-1, ""); }
    }
}
