// The macOS install steps (the shared ones are in shared/EngineBase.cs): a private Python 3.14, Apple's Command Line
// Tools, devkitPro's own macOS pacman with the Switch packages (one password window), and the copy to a mounted SD
// card. Nothing goes through Homebrew or Docker: make_sd.py builds natively (docs/mac-installer-plan.md).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace SwitchWakerHD.Installer;

sealed class MacEngine : EngineBase
{
    // ~/SwitchWakerHD-Build: macOS short user names have no spaces, which the build tools need
    public static readonly string WorkDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "SwitchWakerHD-Build");
    protected override string Work => WorkDir;

    // python-build-standalone (Astral): a relocatable CPython for the user's own folder, no administrator. 3.14 for
    // the minimap's Dolphin .rvz (compression.zstd); its own OpenSSL finds the Mac's certificates.
    const string PyRelease = "20260929", PyVersion = "3.14.7";
    static readonly Dictionary<Architecture, (string triple, string sha256)> PyBuilds = new()
    {
        [Architecture.Arm64] = ("aarch64-apple-darwin", "0211ccf04c5e0a0feb0b9bf1148030e1f6a31b8eae624f13919a7752a4e3dc79"),
        [Architecture.X64]   = ("x86_64-apple-darwin",  "18b12f5836c6a5cee8223f1759d26bd3174c1271bb3b7882dc09e2d961f3c863"),
    };
    static string PyDir => Path.Combine(WorkDir, "python");
    static string PyExe => Path.Combine(PyDir, "bin", "python3");

    // devkitPro's macOS installer: pacman for arm64 and x86_64, the dkp-libs and dkp-osx repositories
    const string DkpPkgUrl = "https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg";
    const string DkpPkgSha = "da74156211cd3d8664ba7fb95544fbc0a2fbef1dd87b1a6bfafb30d2d446cd30";
    const string Dkp = "/opt/devkitpro";
    // the same packages as the Windows installer (cmake and make come from pip and the Command Line Tools)
    const string DkpPackages = "switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake";
    // run through devkitPro's wrapper: it puts its gpg on PATH (the bare pacman fails with "Invalid crypto engine")
    const string DkpPacman = "/usr/local/bin/dkp-pacman";

    Process? current;
    readonly object currentLock = new();

    // ── Python ───────────────────────────────────────────────────────────────
    protected override void Python()
    {
        var arch = RuntimeInformation.OSArchitecture;
        if (!PyBuilds.TryGetValue(arch, out var build)) throw new InstallException(Str.Get("py_mac_arch", arch));
        if (!PythonWorks())
        {
            var name = $"cpython-{PyVersion}+{PyRelease}-{build.triple}-install_only.tar.gz";
            var tgz = Path.Combine(WorkDir, name);
            if (!File.Exists(tgz))
            {
                Say(Str.Get("py_mac_downloading", PyVersion));
                var part = tgz + ".part";
                Download($"https://github.com/astral-sh/python-build-standalone/releases/download/{PyRelease}/{Uri.EscapeDataString(name)}", part);
                VerifySha256(part, build.sha256);
                File.Move(part, tgz);
            }
            Say(Str.Get("py_mac_unpacking"));
            if (Directory.Exists(PyDir)) Directory.Delete(PyDir, true);
            // the archive holds python/: unpacked next to it, as WorkDir/python
            Exec("/usr/bin/tar", $"-xzf {Quote(tgz)} -C {Quote(WorkDir)}");
            if (!PythonWorks()) throw new InstallException(Str.Get("py_mac_fail", PyDir));
        }
        pythonCmd = PyExe;
        Ok(Str.Get("py_found", $"Python {PyVersion} ({PyDir})"));

        Say(Str.Get("py_pkgs"));
        // cmake: the host's (no Homebrew); Pillow: the HOME-screen icon and the NRO's own icon
        const string pkgs = "zstandard pycryptodome pillow cmake";
        if (Exec(pythonCmd, "-m pip install --disable-pip-version-check --no-warn-script-location " + pkgs, check: false) != 0)
            throw new InstallException(Str.Get("py_pkgs_fail", pkgs));
    }

    static bool PythonWorks() =>
        File.Exists(PyExe) && Capture(PyExe, "-c \"import compression.zstd,ssl;print('PYOK')\"").output.Contains("PYOK");

    // ── Apple's Command Line Tools and devkitPro ─────────────────────────────
    protected override void Toolchain()
    {
        CommandLineTools();
        DevkitPro();
    }

    // make and git for the build, a C compiler for the forwarder's packer: Apple's own installer window
    void CommandLineTools()
    {
        Say(Str.Get("clt_checking"));
        if (HaveCommandLineTools()) { Ok(Str.Get("clt_ok")); return; }
        Capture("/usr/bin/xcode-select", "--install");
        while (!HaveCommandLineTools())
        {
            AskUser?.Invoke(Str.Get("clt_notice_h"), Str.Get("clt_notice"), Str.Get("btn_continue"));
            if (!HaveCommandLineTools())
            {
                Warn(Str.Get("clt_missing"));
                Capture("/usr/bin/xcode-select", "--install");   // the window again, if it was closed
            }
        }
        Ok(Str.Get("clt_ok"));
    }

    // /usr/bin/make and /usr/bin/git are stubs that open Apple's installer until the tools are there
    static bool HaveCommandLineTools() =>
        Capture("/usr/bin/xcode-select", "-p").code == 0 &&
        Capture("/usr/bin/xcrun", "--find make").code == 0 && Capture("/usr/bin/xcrun", "--find clang").code == 0;

    static readonly (string what, string path)[] DkpFiles =
    {
        ("devkitA64", Dkp + "/devkitA64/bin/aarch64-none-elf-gcc"),
        ("libnx", Dkp + "/libnx/lib/libnx.a"),
        ("switch-cmake", Dkp + "/cmake/Switch.cmake"),
        ("switch-tools", Dkp + "/tools/bin/elf2nro"),
        ("switch-zlib", Dkp + "/portlibs/switch/lib/libz.a"),
        ("switch-lz4", Dkp + "/portlibs/switch/lib/liblz4.a"),
    };

    static List<string> MissingDevkit() => DkpFiles.Where(f => !File.Exists(f.path)).Select(f => f.what).ToList();

    void DevkitPro()
    {
        Say(Str.Get("dkp_searching"));
        var missing = MissingDevkit();
        if (missing.Count == 0 && File.Exists(DkpPacman)) { Ok(Str.Get("dkp_found_at", Dkp)); return; }

        // One script as root, so macOS asks for the password once: devkitPro's pkg (when pacman is not there yet),
        // then the Switch packages. Its output goes to a log the installer follows.
        string? pkg = null;
        if (!File.Exists(Dkp + "/pacman/bin/pacman") || !File.Exists(DkpPacman))
        {
            pkg = Path.Combine(WorkDir, "devkitpro-pacman-installer.pkg");
            if (!File.Exists(pkg))
            {
                Say(Str.Get("dkp_downloading"));
                var part = pkg + ".part";
                Download(DkpPkgUrl, part);
                Say(Str.Get("dkp_verifying"));
                VerifySha256(part, DkpPkgSha);
                File.Move(part, pkg);
            }
        }
        var log = Path.Combine(WorkDir, "devkitpro-root.log");
        var script = Path.Combine(WorkDir, "devkitpro-root.sh");
        var sb = new StringBuilder();
        sb.Append("#!/bin/sh\n# SwitchWakerHD installer: devkitPro and its Switch packages (run once as root)\ncd /\n");
        sb.Append($"exec > {ShQuote(log)} 2>&1\n");
        if (pkg != null) sb.Append($"/usr/sbin/installer -pkg {ShQuote(pkg)} -target / || exit 10\n");
        // pkg.devkitpro.org often answers 403 to the refresh (as on Windows): then install from the lists already there
        sb.Append($"{DkpPacman} -Sy --needed --noconfirm {DkpPackages} && exit 0\n");
        sb.Append($"echo '== retry without refreshing the package lists'\n{DkpPacman} -S --needed --noconfirm {DkpPackages}\n");
        File.WriteAllText(script, sb.ToString());
        File.Delete(log);

        Say(Str.Get("dkp_mac_password"));
        var prompt = Str.Get("dkp_mac_prompt").Replace("\\", "\\\\").Replace("\"", "\\\"");
        var apple = $"do shell script \"/bin/sh '{script.Replace("'", "'\\''")}'\" with administrator privileges with prompt \"{prompt}\"";
        var output = new List<string>();
        int rc;
        using (var follow = FollowLog(log, output))
            rc = Exec("/usr/bin/osascript", "-e " + Quote(apple), check: false, onLine: output.Add);
        if (rc != 0 && output.Any(l => l.Contains("(-128)")))
            throw new InstallException(Str.Get("dkp_mac_cancelled"));

        missing = MissingDevkit();
        if (missing.Count > 0)
        {
            var why = PacmanFailure(output);
            throw new InstallException(Str.Get("dkp_missing", string.Join(", ", missing)) + " " + Str.Get(why));
        }
        if (rc != 0) Warn(Str.Get("dkp_pkg_warn") + " " + Str.Get(PacmanFailure(output)));
        Ok(Str.Get("dkp_tools_ok"));
    }

    // Streams the root script's log (osascript itself prints nothing until it ends) to the installer's log.
    IDisposable FollowLog(string path, List<string> into)
    {
        var stop = new CancellationTokenSource();
        var thread = new Thread(() =>
        {
            long pos = 0;
            var pending = "";
            while (true)
            {
                var last = stop.IsCancellationRequested;
                try
                {
                    if (File.Exists(path))
                    {
                        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                        if (f.Length > pos)
                        {
                            f.Seek(pos, SeekOrigin.Begin);
                            var buf = new byte[f.Length - pos];
                            var n = f.Read(buf, 0, buf.Length);
                            pos += n;
                            pending += Encoding.UTF8.GetString(buf, 0, n);
                            var lines = pending.Split('\n');
                            pending = lines[lines.Length - 1];
                            foreach (var l in lines.Take(lines.Length - 1))
                            {
                                var line = l.TrimEnd('\r');
                                lock (into) into.Add(line);
                                OnLog(line, LogKind.Output);
                            }
                        }
                    }
                }
                catch (IOException) { }
                if (last) break;
                Thread.Sleep(300);
            }
        }) { IsBackground = true };
        thread.Start();
        return new Stopper(() => { stop.Cancel(); thread.Join(2000); });
    }

    sealed class Stopper : IDisposable
    {
        readonly Action action;
        public Stopper(Action a) => action = a;
        public void Dispose() => action();
    }

    static string ShQuote(string s) => "'" + s.Replace("'", "'\\''") + "'";

    // ── Build ────────────────────────────────────────────────────────────────
    // The native build (make_sd.py picks a container when docker or podman is on PATH: they are left out), with
    // the installer's Python and pip's cmake first.
    protected override void BuildEnv(Dictionary<string, string> env)
    {
        env["DEVKITPRO"] = Dkp;
        env["WWHD_NATIVE"] = "1";
        env["PATH"] = string.Join(":", Path.Combine(PyDir, "bin"), Dkp + "/tools/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin");
    }

    protected override double TotalRamGb()
    {
        var (code, output) = Capture("/usr/sbin/sysctl", "-n hw.memsize");
        return code == 0 && long.TryParse(output.Trim(), out var bytes) ? bytes / 1073741824.0 : 0;
    }

    // ── SD card (from the done page) ─────────────────────────────────────────
    public sealed record Volume(string Mount, string Name, long Size, string Format);

    // Mounted volumes that can be the Switch's card: under /Volumes, ejectable (a card reader, hekate's UMS) or
    // holding the Switch's folders; never the Mac's own disk.
    public static List<Volume> SdVolumes()
    {
        var found = new List<Volume>();
        foreach (var d in DriveInfo.GetDrives())
        {
            try
            {
                var mount = d.RootDirectory.FullName.TrimEnd('/');
                if (!d.IsReady || !mount.StartsWith("/Volumes/")) continue;
                var switchy = Directory.Exists(Path.Combine(mount, "atmosphere")) || Directory.Exists(Path.Combine(mount, "switch")) ||
                              Directory.Exists(Path.Combine(mount, "bootloader"));
                var info = Capture("/usr/sbin/diskutil", "info " + Quote(mount)).output;
                var ejectable = Regex.IsMatch(info, @"^\s*(Ejectable|Removable Media):\s*(Yes|Removable)", RegexOptions.Multiline);
                var internalDisk = Regex.IsMatch(info, @"^\s*(Device Location|Internal):\s*(Internal|Yes)", RegexOptions.Multiline);
                var isDmg = Regex.IsMatch(info, @"Disk Image|Protocol:\s*Disk Image", RegexOptions.IgnoreCase);
                if (isDmg || (!ejectable && !switchy) || (internalDisk && !switchy)) continue;
                found.Add(new Volume(mount, d.VolumeLabel is { Length: > 0 } l ? l : Path.GetFileName(mount), d.TotalSize, d.DriveFormat));
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
        }
        return found;
    }

    // A plain copy, file by file (no extended attributes: no ._ files on the card), of what is new or changed;
    // progress(done bytes, total bytes).
    public void CopyToSd(string mount, Action<long, long> progress, CancellationToken token)
    {
        ct = token;
        Say(Str.Get("sd_copying", mount));
        var files = Directory.GetFiles(SdOut, "*", SearchOption.AllDirectories)
            .Where(f => !Path.GetFileName(f).StartsWith("._") && Path.GetFileName(f) != ".DS_Store").ToList();
        var di = new DriveInfo(mount);
        long need = 0, biggest = 0, total = 0;
        var todo = new List<(string src, string dst, long len)>();
        foreach (var f in files)
        {
            var fi = new FileInfo(f);
            var dst = new FileInfo(Path.Combine(mount, Path.GetRelativePath(SdOut, f)));
            biggest = Math.Max(biggest, fi.Length);
            if (dst.Exists && dst.Length == fi.Length && dst.LastWriteTimeUtc >= fi.LastWriteTimeUtc) continue;
            need += Math.Max(0, fi.Length - (dst.Exists ? dst.Length : 0));
            total += fi.Length;
            todo.Add((f, dst.FullName, fi.Length));
        }
        if (di.AvailableFreeSpace < need)
            throw new InstallException(Str.Get("sd_no_space", mount, (di.AvailableFreeSpace / 1e9).ToString("0.0"), (need / 1e9).ToString("0.0")));
        if (biggest >= 4L << 30 && di.DriveFormat.Equals("msdos", StringComparison.OrdinalIgnoreCase))
            throw new InstallException(Str.Get("sd_fat32", mount));
        if (!Directory.Exists(Path.Combine(mount, "atmosphere"))) Warn(Str.Get("sd_no_atmo", mount));
        Info(Str.Get("sd_mac_todo", todo.Count, files.Count - todo.Count));

        long done = 0;
        var buf = new byte[1 << 20];
        var last = DateTime.MinValue;
        try
        {
            foreach (var (src, dst, _) in todo)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
                using (var input = File.OpenRead(src))
                using (var output = new FileStream(dst, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                {
                    int n;
                    while ((n = input.Read(buf, 0, buf.Length)) > 0)
                    {
                        ct.ThrowIfCancellationRequested();
                        output.Write(buf, 0, n);
                        done += n;
                        if ((DateTime.UtcNow - last).TotalMilliseconds > 200) { last = DateTime.UtcNow; progress(done, total); }
                    }
                }
                File.SetLastWriteTimeUtc(dst, File.GetLastWriteTimeUtc(src));
            }
        }
        catch (IOException e) { throw new InstallException(Str.Get("sd_fail_mac", e.Message)); }
        progress(total, total);
        Capture("/bin/sync", "");
        Ok(Str.Get("sd_copied", mount));
    }

    public static bool Eject(string mount) => Capture("/usr/sbin/diskutil", "eject " + Quote(mount)).code == 0;

    // ── Processes ────────────────────────────────────────────────────────────
    protected override int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null,
                                IDictionary<string, string>? env = null)
    {
        OnLog($"> {exe} {args}", LogKind.Info);
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute        = false,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding  = Encoding.UTF8,
            WorkingDirectory       = workDir ?? WorkDir,
        };
        psi.Environment["PYTHONUNBUFFERED"] = "1";
        psi.Environment["PYTHONIOENCODING"] = "utf-8";
        psi.Environment["LC_ALL"] = "en_US.UTF-8";   // English tool output, for the failure hints
        if (env != null) foreach (var kv in env) psi.Environment[kv.Key] = kv.Value;

        int rc;
        var gate = new object();   // stdout and stderr arrive on two threads; onLine sees one line at a time
        try
        {
            using var proc = new Process { StartInfo = psi };
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) lock (gate) { OnLog(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.ErrorDataReceived  += (_, e) => { if (e.Data != null) lock (gate) { OnLog(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.Start();
            lock (currentLock) current = proc;
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();
            while (!proc.WaitForExit(250))
                if (ct.IsCancellationRequested) { KillCurrent(); ct.ThrowIfCancellationRequested(); }
            proc.WaitForExit();   // the redirected output, to its end
            ct.ThrowIfCancellationRequested();   // killed by KillCurrent: a cancel, not a failure
            rc = proc.ExitCode;
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception e)
        {
            OnLog(e.Message, LogKind.Error);
            rc = -1;
        }
        finally { lock (currentLock) current = null; }

        if (check && rc != 0) throw new InstallException(Str.Get("cmd_fail", Path.GetFileName(exe), rc));
        return rc;
    }

    // Stops the running process and everything it started (python, make, the compilers).
    public override void KillCurrent()
    {
        lock (currentLock)
        {
            try { if (current != null && !current.HasExited) current.Kill(entireProcessTree: true); }
            catch (Exception) { }
        }
    }

    public static void Open(string path, string? app = null)
    {
        try
        {
            var psi = new ProcessStartInfo("/usr/bin/open") { UseShellExecute = false };
            if (app != null) { psi.ArgumentList.Add("-a"); psi.ArgumentList.Add(app); }
            psi.ArgumentList.Add(path);
            Process.Start(psi)?.Dispose();
        }
        catch (Exception) { }
    }
}
