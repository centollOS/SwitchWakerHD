// The install steps both installers share (Windows: windows/InstallEngine.cs, Linux: linux/LinuxEngine.cs):
// the release download and the build, with its progress. Each system brings its own Python and Switch
// toolchain steps and its own way to run and stop processes. Everything runs off the UI thread: output goes
// to events, and failures throw InstallException. Builds for .NET Framework 4.8 and .NET 8 alike.
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;

namespace SwitchWakerHD.Installer;

enum StepState { Pending, Running, Done, Failed }
enum LogKind { Info, Ok, Warn, Error, Output }

sealed class InstallException : Exception
{
    public InstallException(string message) : base(message) { }
}

abstract class EngineBase
{
    public static readonly string WorkDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "SwitchWakerHD-Build");

    public const int StepPython = 0, StepDevkit = 1, StepRelease = 2, StepBuild = 3;

    public event Action<string, LogKind>? Log;
    public event Action<int, StepState>? StepChanged;
    public event Action<string>? Status;
    public event Action<double?>? Progress;          // 0..1, or null for "busy, no estimate"
    // Shows a notice (title, text, button, and a command for the user to run, or null) and blocks this
    // thread until the user presses the button.
    public Action<string, string, string, string?>? AskUser;

    public string GamePath = "";
    public string GameArg  = "";                      // --image, --wua or --game-dir
    // Optional (Windows): the console's prod.keys, for the HOME-screen forwarder (.nsp). Only with it are Git,
    // Pillow and MSYS2's gcc installed (INSTALL.md, "HOME-screen icon"); without it make_sd.py gets
    // --no-forwarder. Linux never sets it: make_sd.py's forwarder step is the Windows script.
    public string? ForwarderKeys { get; set; }

    public string ReleaseDir { get; protected set; } = "";
    public string SdOut => Path.Combine(ReleaseDir, "build", "sd");

    protected string pythonCmd = "python";
    protected string releaseVer = "v0.4.3";
    protected CancellationToken ct;

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
        RunStep(StepDevkit,  Toolchain);
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

    // Python 3 with zstandard and pycryptodome; sets pythonCmd.
    protected abstract void Python();
    // The Switch toolchain make_sd.py builds with.
    protected abstract void Toolchain();
    // Runs a process, streams its output to the log (and to onLine); with check, a non-zero exit is a
    // failure. A cancel stops it and everything it started, and throws OperationCanceledException.
    protected abstract int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null);
    public abstract void KillCurrent();

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

    // ── Helpers ──────────────────────────────────────────────────────────────
    protected void Download(string url, string dest)
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

    protected void VerifySha256(string file, string expected)
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

    protected void Ask(string heading, string text, string button, string? command = null) =>
        AskUser?.Invoke(heading, text, button, command);

    protected void Say(string msg)  { Status?.Invoke(msg); Log?.Invoke(msg, LogKind.Info); }
    protected void Info(string msg) => Log?.Invoke(msg, LogKind.Info);
    protected void Ok(string msg)   => Log?.Invoke(msg, LogKind.Ok);
    protected void Warn(string msg) => Log?.Invoke(msg, LogKind.Warn);
    protected void LogLine(string msg, LogKind kind) => Log?.Invoke(msg, kind);
    protected void SetProgress(double? p) => Progress?.Invoke(p);
}
