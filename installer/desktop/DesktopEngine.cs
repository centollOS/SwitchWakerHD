// What the macOS and Linux installers share (MacEngine.cs, LinuxEngine.cs; the steps every installer shares are in
// shared/EngineBase.cs): the work folder, a private Python 3.14, processes and their cancel, and the copy to a
// mounted SD card.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace SwitchWakerHD.Installer;

abstract class DesktopEngine : EngineBase
{
    // ~/SwitchWakerHD-Build (macOS short user names and Linux user names have no spaces, which the build tools need)
    public static readonly string WorkDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "SwitchWakerHD-Build");
    protected override string Work => WorkDir;

    public static DesktopEngine Create() => OperatingSystem.IsMacOS() ? new MacEngine() : new LinuxEngine();

    // A notice with a command for the user to run in a terminal (Linux without a password window): heading, text,
    // button, command. Blocks this thread until the button.
    public Action<string, string, string, string>? AskCommand;

    // ── Python ───────────────────────────────────────────────────────────────
    // python-build-standalone (Astral): a relocatable CPython for the user's own folder, no administrator, no
    // python3-venv, no pip on the system's Python (PEP 668). 3.14 for the minimap's Dolphin .rvz (compression.zstd).
    const string PyRelease = "20260929", PyVersion = "3.14.7";
    static readonly Dictionary<(bool mac, Architecture arch), (string triple, string sha256)> PyBuilds = new()
    {
        [(true,  Architecture.Arm64)] = ("aarch64-apple-darwin",      "0211ccf04c5e0a0feb0b9bf1148030e1f6a31b8eae624f13919a7752a4e3dc79"),
        [(true,  Architecture.X64)]   = ("x86_64-apple-darwin",       "18b12f5836c6a5cee8223f1759d26bd3174c1271bb3b7882dc09e2d961f3c863"),
        [(false, Architecture.Arm64)] = ("aarch64-unknown-linux-gnu", "08a224bc0bcadfee3eab55db618343e6ba221f3595000e0894aae163648abc9d"),
        [(false, Architecture.X64)]   = ("x86_64-unknown-linux-gnu",  "0056208b5fdfcec939bc5b3908f5e7c89d11c6e2f1fdf02c9cc6aa98d10c5d05"),
    };
    protected static string PyDir => Path.Combine(WorkDir, "python");
    static string PyExe => Path.Combine(PyDir, "bin", "python3");

    protected override void Python()
    {
        var arch = RuntimeInformation.OSArchitecture;
        if (!PyBuilds.TryGetValue((OperatingSystem.IsMacOS(), arch), out var build))
            throw new InstallException(Str.Get("py_desk_arch", arch));
        if (!PythonWorks())
        {
            var name = $"cpython-{PyVersion}+{PyRelease}-{build.triple}-install_only.tar.gz";
            var tgz = Path.Combine(WorkDir, name);
            if (!File.Exists(tgz))
            {
                Say(Str.Get("py_desk_downloading", PyVersion));
                var part = tgz + ".part";
                Download($"https://github.com/astral-sh/python-build-standalone/releases/download/{PyRelease}/{Uri.EscapeDataString(name)}", part);
                VerifySha256(part, build.sha256);
                File.Move(part, tgz);
            }
            Say(Str.Get("py_desk_unpacking"));
            if (Directory.Exists(PyDir)) Directory.Delete(PyDir, true);
            // the archive holds python/: unpacked next to it, as WorkDir/python
            Exec("tar", $"-xzf {Quote(tgz)} -C {Quote(WorkDir)}");
            if (!PythonWorks()) throw new InstallException(Str.Get("py_desk_fail", PyDir));
        }
        pythonCmd = PyExe;
        Ok(Str.Get("py_found", $"Python {PyVersion} ({PyDir})"));

        Say(Str.Get("py_pkgs"));
        // cmake: the host's (no Homebrew, no distribution package); Pillow: the HOME-screen icon and the NRO's own;
        // ninja: the prepared graphics' tools/switch/dksh_cache build (issue #88)
        const string pkgs = "zstandard pycryptodome pillow cmake ninja";
        if (Exec(pythonCmd, "-m pip install --disable-pip-version-check --no-warn-script-location " + pkgs, check: false) != 0)
            throw new InstallException(Str.Get("py_pkgs_fail", pkgs));
    }

    static bool PythonWorks() =>
        File.Exists(PyExe) && Capture(PyExe, "-c \"import compression.zstd,ssl;print('PYOK')\"").output.Contains("PYOK");

    // ── devkitPro ───────────────────────────────────────────────────────────
    // What the native build needs from devkitPro, by its files (the same packages on macOS and Linux).
    // devkitA64 by name: it is now a metapackage over devkita64-gcc, -binutils and -newlib, and switch-dev brings
    // only -binutils. An older devkitA64 owns those files itself; without its upgrade in the same transaction,
    // installing -binutils stops with "conflicting files" (#28).
    protected const string Dkp = "/opt/devkitpro";
    protected const string DkpPackages = "devkitA64 switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake";
    static readonly (string what, string path)[] DkpFiles =
    {
        ("devkitA64", Dkp + "/devkitA64/bin/aarch64-none-elf-gcc"),
        ("libnx", Dkp + "/libnx/lib/libnx.a"),
        ("switch-cmake", Dkp + "/cmake/Switch.cmake"),
        ("switch-tools", Dkp + "/tools/bin/elf2nro"),
        ("switch-zlib", Dkp + "/portlibs/switch/lib/libz.a"),
        ("switch-lz4", Dkp + "/portlibs/switch/lib/liblz4.a"),
    };

    protected static int DkpFilesCount => DkpFiles.Length;
    protected static List<string> MissingDevkit() => DkpFiles.Where(f => !File.Exists(f.path)).Select(f => f.what).ToList();

    // For the root scripts: a shell function, dkp_xfer, that installs the Switch packages with pacman downloading
    // through the installer's Python (EngineBase.WritePacmanFetch), from a copy of devkitPro's pacman.conf that
    // the script makes then (on a new install the file only appears in the same script).
    protected string PacmanXferFunction(string pacman)
    {
        var xfer = $"XferCommand = \"{PyExe}\" \"{WritePacmanFetch()}\" / %u %o";
        return "dkp_xfer() {\n" +
               "  echo '== retry with the downloads through the installer'\"'\"'s Python'\n" +
               "  c=$(mktemp) || return 1\n" +
               $"  awk -v x={ShQuote(xfer)} '{{ print }} /^\\[options\\]/ {{ print x }}' {Dkp}/pacman/etc/pacman.conf > \"$c\" &&\n" +
               $"    {pacman} --config \"$c\" -Sy --needed --noconfirm {DkpPackages}\n" +
               "  rc=$?; rm -f \"$c\"; return $rc\n" +
               "}\n";
    }

    // ── SD card (from the done page) ─────────────────────────────────────────
    public sealed record Volume(string Mount, string Name, long Size, string Format);

    // The mounted volumes that can be the Switch's card, never the computer's own disks.
    public abstract List<Volume> SdVolumes();
    public abstract bool Eject(string mount);
    // Opens a folder in the file manager, or a text file (the log) in an editor.
    public abstract void Open(string path, bool text = false);

    // A plain copy, file by file (no extended attributes: no ._ files from a Mac), of what is new or changed, then
    // every file read back (VerifyCard); progress(done bytes, total bytes, checking).
    public void CopyToSd(string mount, Action<long, long, bool> progress, CancellationToken token)
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
            // FAT and exFAT keep times to 2 s
            if (dst.Exists && dst.Length == fi.Length && dst.LastWriteTimeUtc >= fi.LastWriteTimeUtc.AddSeconds(-2)) continue;
            need += Math.Max(0, fi.Length - (dst.Exists ? dst.Length : 0));
            total += fi.Length;
            todo.Add((f, dst.FullName, fi.Length));
        }
        if (di.AvailableFreeSpace < need)
            throw new InstallException(Str.Get("sd_no_space", mount, (di.AvailableFreeSpace / 1e9).ToString("0.0"), (need / 1e9).ToString("0.0")));
        if (biggest >= 4L << 30 && (di.DriveFormat.Equals("msdos", StringComparison.OrdinalIgnoreCase) ||
                                    di.DriveFormat.Equals("vfat", StringComparison.OrdinalIgnoreCase)))
            throw new InstallException(Str.Get("sd_fat32", mount));
        if (!Directory.Exists(Path.Combine(mount, "atmosphere"))) Warn(Str.Get("sd_no_atmo", mount));
        Info(Str.Get("sd_desk_todo", todo.Count, files.Count - todo.Count));

        long done = 0;
        var last = DateTime.MinValue;
        try
        {
            foreach (var (src, dst, _) in todo)
                CopyFile(src, dst, n =>
                {
                    done += n;
                    if ((DateTime.UtcNow - last).TotalMilliseconds > 200) { last = DateTime.UtcNow; progress(done, total, false); }
                });
            progress(total, total, false);
            Capture("sync", "");
            VerifyCard(mount, files, OpenCard, (src, dst) => CopyFile(src, dst, null), (d, t) => progress(d, t, true));
        }
        catch (IOException e) { throw new InstallException(Str.Get("sd_fail_desk", e.Message)); }
        Ok(Str.Get("sd_copied", mount));
    }

    // One file to the card, written past the system's cache (Uncache), so the check after reads the card
    void CopyFile(string src, string dst, Action<long>? onBytes)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
        var buf = new byte[1 << 20];
        using (var input = File.OpenRead(src))
        using (var output = new FileStream(dst, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
        {
            Uncache(output);
            int n;
            while ((n = input.Read(buf, 0, buf.Length)) > 0)
            {
                ct.ThrowIfCancellationRequested();
                output.Write(buf, 0, n);
                onBytes?.Invoke(n);
            }
            output.Flush(true);
            Uncache(output);
        }
        File.SetLastWriteTimeUtc(dst, File.GetLastWriteTimeUtc(src));
    }

    FileStream OpenCard(string path)
    {
        var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        Uncache(s);
        return s;
    }

    // Keeps a card file's data out of the system's cache (macOS: F_NOCACHE), or drops what it holds of it once
    // written to the card (Linux: posix_fadvise DONTNEED); called on opening and after a write's flush.
    protected abstract void Uncache(FileStream file);

    // ── Processes ────────────────────────────────────────────────────────────
    Process? current;
    readonly object currentLock = new();

    protected override int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null,
                                IDictionary<string, string>? env = null)
    {
        OnLog($"> {exe} {args}", LogKind.Info);
        var psi = new ProcessStartInfo(exe, args)
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
        psi.Environment["LC_ALL"] = OperatingSystem.IsMacOS() ? "en_US.UTF-8" : "C.UTF-8";   // English tool output, for the failure hints
        if (env != null) foreach (var kv in env) psi.Environment[kv.Key] = kv.Value;

        int rc;
        var gate = new object();   // stdout and stderr arrive on two threads; onLine sees one line at a time
        try
        {
            using var proc = new Process { StartInfo = psi };
            proc.OutputDataReceived += (_, e) => { if (e.Data != null) lock (gate) { OnLog(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.ErrorDataReceived  += (_, e) => { if (e.Data != null) lock (gate) { OnLog(e.Data, LogKind.Output); onLine?.Invoke(e.Data); } };
            proc.Start();
            proc.StandardInput.Close();
            lock (currentLock) current = proc;
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();
            while (!proc.WaitForExit(250))
                if (ct.IsCancellationRequested) { KillCurrent(); ct.ThrowIfCancellationRequested(); }
            // the redirected output, to its end; bounded, in case something it started keeps the pipes open
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

    // Streams a log file another process writes (a root script: pkexec and osascript print nothing of it) to the
    // installer's log, and into `into`; stops when disposed.
    protected IDisposable FollowLog(string path, List<string> into)
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

    protected static string ShQuote(string s) => "'" + s.Replace("'", "'\\''") + "'";

    protected static string? FindOnPath(string name) =>
        (Environment.GetEnvironmentVariable("PATH") ?? "").Split(':')
            .Where(d => d.Length > 0).Select(d => Path.Combine(d, name)).FirstOrDefault(File.Exists);
}
