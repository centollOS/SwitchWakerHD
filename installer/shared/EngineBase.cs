// The install steps every installer shares (Windows: windows/InstallEngine.cs, macOS: desktop/MacEngine.cs):
// the checks before anything, the release download and the build, with its progress and its failure hints. Each
// system brings its own Python and Switch toolchain steps and its own way to run and stop processes. Everything
// runs off the UI thread: output goes to events, and failures throw InstallException. Builds for .NET Framework
// 4.8 (Windows) and .NET 8 (macOS) alike.
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

abstract class EngineBase
{
    // The work folder: the release, the build and the log go there.
    protected abstract string Work { get; }

    public const int StepPython = 0, StepDevkit = 1, StepRelease = 2, StepBuild = 3;

    public event Action<string, LogKind>? Log;
    public event Action<int, StepState>? StepChanged;
    public event Action<string>? Status;
    public event Action<double?>? Progress;          // 0..1, or null for "busy, no estimate"
    // Shows a notice (title, text, button) and blocks this thread until the user presses the button.
    public Action<string, string, string>? AskUser;
    protected void OnLog(string msg, LogKind kind) => Log?.Invoke(msg, kind);
    protected void OnStatus(string msg) => Status?.Invoke(msg);
    protected void OnProgress(double? p) => Progress?.Invoke(p);

    public string GamePath = "";
    public string GameArg  = "";                      // --image, --wua or --game-dir
    // Optional: the console's prod.keys, for the HOME-screen forwarder (.nsp). Only with it are Git, Pillow
    // and MSYS2's gcc installed (INSTALL.md, "HOME-screen icon"); without it make_sd.py gets --no-forwarder.
    public string? ForwarderKeys;
    // Optional: the player's GameCube Wind Waker (disc image or folder), for the GameCube sea minimap: make_sd.py
    // downloads the mod from the mod catalogue and builds it into the game (INSTALL.md, "Optional: code mods").
    public string? MinimapGame;
    public const string MinimapMod = "gc-minimap";
    // "Prepare the graphics now" (issue #88): the release's shader template (shader-template.sealed, shader states
    // without game code, opened with the game's own files) becomes shadercache_dksh.bin on this computer
    // (make_sd.py --shaders), so the console's first start compiles almost nothing. Never a reason to stop: a
    // release without it, a download or build that fails, or a game it does not open with only leave the console to
    // compile its shaders as it always did. On Windows it also needs MSYS2's gcc, ninja and zlib-devel.
    public bool PrepareShaders = true;
    string? shaderTemplate;

    // What make_sd.py's check of the game's content/ found (tools/switch/game_files.py): how many files are missing,
    // cut off or damaged, and its lines naming them. A warning: the build goes on (#48).
    public int BadGameFiles { get; private set; }
    public List<string> BadGameFileLines { get; } = new();

    public string ReleaseDir { get; private set; } = "";
    public string SdOut => Path.Combine(ReleaseDir, "build", "sd");

    protected string pythonCmd = "python";
    string releaseVer = "";
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
        Directory.CreateDirectory(Work);
        Info(Str.Get("welcome_dir", Work));
        Preflight();
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

    // Python 3 with the packages make_sd.py needs; sets pythonCmd.
    protected abstract void Python();
    // The Switch toolchain make_sd.py builds with.
    protected abstract void Toolchain();
    // Runs a process, streams its output to the log (and to onLine); with check, a non-zero exit is a failure. A
    // cancel stops it and everything it started, and throws OperationCanceledException.
    protected abstract int Exec(string exe, string args, string? workDir = null, bool check = true, Action<string>? onLine = null,
                                IDictionary<string, string>? env = null);
    public abstract void KillCurrent();
    // The computer's memory in GB, or 0 when the system does not say.
    protected abstract double TotalRamGb();
    // What the system adds to make_sd.py's environment (macOS: the native devkitPro and the installer's Python).
    protected virtual void BuildEnv(Dictionary<string, string> env) { }

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
        var rerun = Directory.Exists(Work) && Directory.GetDirectories(Work, "SwitchWakerHD-v*").Length > 0;
        var needGb = rerun ? 5 : 10;
        try
        {
            var drive = new DriveInfo(Path.GetPathRoot(Work)!);
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
        catch (HttpRequestException e) when ((e.InnerException is WebException we &&
                                              (we.Status == WebExceptionStatus.TrustFailure || we.Status == WebExceptionStatus.SecureChannelFailure)) ||
                                             e.InnerException is System.Security.Authentication.AuthenticationException)   // .NET 8's
        {
            throw new InstallException(Str.Get("pre_tls", e.InnerException!.Message));
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

    // Why pacman (devkitPro's: MSYS2 on Windows, its own on macOS) failed, from its output: a Str key that explains
    // it to the player.
    internal static string PacmanFailure(IEnumerable<string> output)
    {
        var all = string.Join("\n", output);
        bool Has(string pattern) => Regex.IsMatch(all, pattern, RegexOptions.IgnoreCase);
        // pacman's curl ("returned error: 403"); wget fetching devkitPro's apt installer on Linux ("ERROR 403")
        return Has(@"returned error: 403|ERROR 403|403 Forbidden")            ? "dkp_why_403" :
               Has(@"certificate has expired|certificate is not yet valid|SSL certificate") ? "dkp_why_ssl" :
               Has(@"unable to lock database")                                 ? "dkp_why_lock" :
               Has(@"returned error: 429")                                     ? "dkp_why_429" :
               Has(@"Could not resolve host|timed out|Failed to connect|Connection reset|returned error: 5\d\d|Recv failure") ? "dkp_why_net" :
               Has(@"signature .* (unknown trust|is invalid)|invalid or corrupted package|keyring") ? "dkp_why_sig" :
               "dkp_why_other";
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
            releaseVer = Directory.GetFileSystemEntries(Work, "SwitchWakerHD-v*")
                .Select(p => Regex.Match(Path.GetFileName(p), @"^SwitchWakerHD-(v(\d+)\.(\d+)\.(\d+))(\.zip)?$"))
                .Where(m => m.Success)
                .OrderByDescending(m => new Version(int.Parse(m.Groups[2].Value), int.Parse(m.Groups[3].Value), int.Parse(m.Groups[4].Value)))
                .Select(m => m.Groups[1].Value).FirstOrDefault() ?? "";
            if (releaseVer == "") throw new InstallException(Str.Get("rel_unknown"));
            Warn(Str.Get("rel_fallback", releaseVer));
        }

        var zip = Path.Combine(Work, $"SwitchWakerHD-{releaseVer}.zip");
        ReleaseDir = Path.Combine(Work, $"SwitchWakerHD-{releaseVer}");
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
        ShaderTemplate();
    }

    // The release's shader template, next to its zip (PrepareShaders): downloaded and checked against its .sha256 once;
    // a release without one, or a failed download, only means no prepared graphics.
    void ShaderTemplate()
    {
        shaderTemplate = null;
        if (!PrepareShaders) return;
        var file = Path.Combine(Work, $"shader-template-{releaseVer}.sealed");
        if (!File.Exists(file))
        {
            var url = $"https://github.com/centollOS/SwitchWakerHD/releases/download/{releaseVer}/shader-template.sealed";
            var part = file + ".part";
            try
            {
                Say(Str.Get("shaders_downloading"));
                Download(url, part);
                Download(url + ".sha256", file + ".sha256");
                VerifySha256(part, File.ReadAllText(file + ".sha256").Split(' ')[0].Trim());
                File.Move(part, file);
            }
            catch (InstallException e)
            {
                try { File.Delete(part); } catch (IOException) { }
                Log?.Invoke(e.Message, LogKind.Info);
                Warn(Str.Get("shaders_none"));
                return;
            }
        }
        shaderTemplate = file;
        Ok(Str.Get("shaders_ready"));
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
        BadGameFiles = 0;
        BadGameFileLines.Clear();
        double best = -1;
        void Report(double p)
        {
            if (p <= best) return;   // parallel jobs can print out of order; the bar only moves forward
            best = p;
            Progress?.Invoke(p);
        }
        var badFiles = new Regex(@"^  WARNING: game files missing or damaged: (\d+)");
        var inBadFiles = false;
        void OnLine(string line)
        {
            Match m;
            if ((m = badFiles.Match(line)).Success) { BadGameFiles = int.Parse(m.Groups[1].Value); inBadFiles = true; return; }
            if (inBadFiles && line.StartsWith("    ")) { BadGameFileLines.Add(line.Trim()); return; }
            inBadFiles = false;
            if (line.StartsWith("[+] HOME-screen forwarder")) { Status?.Invoke(Str.Get("fwd_building")); return; }
            if (line.StartsWith("  " + MinimapMod + ": ")) { Status?.Invoke(Str.Get("mm_building")); return; }
            // make_sd.py --shaders: the template becomes the console's shader cache (a few minutes)
            if (line.Contains("shader_manifest.py from-template")) { Status?.Invoke(Str.Get("shaders_preparing")); return; }
            if (line.StartsWith("make_sd: warning: the graphics were not prepared")) { Warn(Str.Get("shaders_failed")); return; }
            m = StageLine.Match(line);
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
        BuildEnv(env);
        var tail = new Queue<string>();
        var forwarder = ForwarderKeys != null ? " --keys " + Quote(ForwarderKeys) : " --no-forwarder";
        var minimap = MinimapGame != null ? $" --guest-mod {MinimapMod} --gc-game {Quote(MinimapGame)}" : "";
        var shaders = shaderTemplate != null ? " --shaders " + Quote(shaderTemplate) : "";
        var rc = Exec(pythonCmd, $"{Quote(MakeSd(ReleaseDir))} {GameArg} {Quote(GamePath)}{forwarder}{minimap}{shaders}", ReleaseDir, check: false,
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
        if (BadGameFiles > 0) Warn(Str.Get("game_files_warn", BadGameFiles));
        Ok(SdOut);
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

    // ── The SD card copy, read back ──────────────────────────────────────────
    // A file already on the card with the same size (and date) is not copied again, damaged or not; and a card can
    // keep something other than what was written (a worn or fake card). So after the copy every file is read back
    // from the card and compared with the SD folder's (SHA-256); one that differs, or is missing, is copied again
    // (copyAgain) and read once more. openCard must open a card file past the system's cache, or what comes back
    // is what was just written to memory. progress(done bytes, total bytes). Throws when files still differ.
    protected void VerifyCard(string card, IList<string> files, Func<string, Stream> openCard,
                              Action<string, string> copyAgain, Action<long, long> progress)
    {
        Say(Str.Get("sd_verifying"));
        long total = files.Sum(f => new FileInfo(f).Length), done = 0;
        var last = DateTime.MinValue;
        void Advance(long n)
        {
            done += n;
            if ((DateTime.UtcNow - last).TotalMilliseconds > 200) { last = DateTime.UtcNow; progress(done, total); }
        }
        bool Same(string src, string dst, Action<long>? onRead)
        {
            if (!File.Exists(dst) || new FileInfo(dst).Length != new FileInfo(src).Length) return false;
            byte[] want;
            using (var s = File.OpenRead(src)) want = Sha256(s, null);
            using (var s = openCard(dst)) return want.SequenceEqual(Sha256(s, onRead));
        }
        var bad = new List<string>();
        foreach (var src in files)
        {
            var rel = src.Substring(SdOut.Length).TrimStart('/', '\\');
            var dst = Path.Combine(card, rel);
            var len = new FileInfo(src).Length;
            long read = 0;
            if (Same(src, dst, n => { read += n; Advance(n); })) continue;
            Advance(len - read);
            Warn(Str.Get("sd_verify_again", rel));
            copyAgain(src, dst);
            if (!Same(src, dst, null)) bad.Add(rel);
        }
        progress(total, total);
        if (bad.Count > 0)
            throw new InstallException(Str.Get("sd_verify_fail", bad.Count, string.Join("\n", bad.Take(5))));
        Ok(Str.Get("sd_verified", files.Count));
    }

    byte[] Sha256(Stream s, Action<long>? onRead)
    {
        using var sha = SHA256.Create();
        var buf = new byte[1 << 20];
        int n;
        while ((n = s.Read(buf, 0, buf.Length)) > 0)
        {
            ct.ThrowIfCancellationRequested();
            sha.TransformBlock(buf, 0, n, null, 0);
            onRead?.Invoke(n);
        }
        sha.TransformFinalBlock(buf, 0, 0);
        return sha.Hash!;
    }

    // ── pacman through the installer's Python ────────────────────────────────
    // devkitPro's servers sit behind Cloudflare's bot rules, which decide by the client, not only by the address
    // (docs/linux-installer-plan.md, "devkitPro's 403"): Python with a pacman user agent passes them. When pacman's
    // own downloads get 403 (#28), it is run again with a copy of its pacman.conf whose XferCommand is this script:
    //   XferCommand = <python> <script> <root> %u %o
    // <root> is MSYS2's folder on Windows, for an %o that reaches a Windows Python as a POSIX path ("/" elsewhere).
    // Its "returned error: 403" lines are the ones PacmanFailure reads in pacman's own output.
    const string PacmanFetchScript = """
        # SwitchWakerHD installer: pacman's XferCommand, the download through Python with a pacman user agent
        import os, platform, shutil, sys, urllib.error, urllib.request
        root, url, out = sys.argv[1:4]
        if os.name == "nt" and out.startswith("/"):
            out = root.rstrip("/") + out
        system = "MSYS_NT-10.0" if os.name == "nt" else platform.system()
        agent = f"pacman/6.0.2 ({system} {platform.machine()}) libalpm/13.0.2"
        try:
            request = urllib.request.Request(url, headers={"User-Agent": agent})
            with urllib.request.urlopen(request, timeout=60) as response, open(out, "wb") as f:
                shutil.copyfileobj(response, f)
        except urllib.error.HTTPError as e:
            print(f"error: failed retrieving file '{os.path.basename(url)}': The requested URL returned error: {e.code}",
                  file=sys.stderr)
        except (OSError, urllib.error.URLError) as e:
            print(f"error: failed retrieving file '{os.path.basename(url)}': {e}", file=sys.stderr)
        else:
            sys.exit(0)
        try:
            os.remove(out)
        except OSError:
            pass
        sys.exit(1)
        """;

    // Writes the script to the work folder; returns its path.
    protected string WritePacmanFetch()
    {
        var path = Path.Combine(Work, "pacman-fetch.py");
        File.WriteAllText(path, PacmanFetchScript.Replace("\r\n", "\n") + "\n");
        return path;
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

    protected static (int code, string output) Capture(string exe, string args)
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


    protected void Say(string msg)  { Status?.Invoke(msg); Log?.Invoke(msg, LogKind.Info); }
    protected void Info(string msg) => Log?.Invoke(msg, LogKind.Info);
    protected void Ok(string msg)   => Log?.Invoke(msg, LogKind.Ok);
    protected void Warn(string msg) => Log?.Invoke(msg, LogKind.Warn);
}
