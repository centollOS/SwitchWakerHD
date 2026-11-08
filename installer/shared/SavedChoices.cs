// The player's choices of the last build that worked (#110): installer-choices.txt in the work folder, written at
// the end of a successful Run. The next start fills the pages with them, and when a newer release is out the welcome
// page offers "Update to vX (same game and options)". Only paths and switches are kept, never what the files hold
// (the console's keys stay in their own file). One "key=value" a line: .NET Framework 4.8 has no JSON reader.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace SwitchWakerHD.Installer;

sealed class SavedChoices
{
    public string GamePath = "";
    public string GameArg = "";           // --image, --wua or --game-dir
    public string? ForwarderKeys;
    public string? MinimapGame;
    public string? PtbrDownload;
    public bool PrepareShaders = true;
    public bool RemoveOldBuilds = true;   // (#120)
    public string Release = "";           // the release that build made

    public const string FileName = "installer-choices.txt";

    public static SavedChoices? Load(string work)
    {
        try
        {
            var path = Path.Combine(work, FileName);
            if (!File.Exists(path)) return null;
            var c = new SavedChoices();
            foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
            {
                var eq = line.IndexOf('=');
                if (eq <= 0) continue;
                var v = line.Substring(eq + 1);
                switch (line.Substring(0, eq))
                {
                    case "game": c.GamePath = v; break;
                    case "format": c.GameArg = v; break;
                    case "forwarder_keys": c.ForwarderKeys = v == "" ? null : v; break;
                    case "minimap_game": c.MinimapGame = v == "" ? null : v; break;
                    case "ptbr_download": c.PtbrDownload = v == "" ? null : v; break;
                    case "prepare_shaders": c.PrepareShaders = v == "1"; break;
                    case "remove_old_builds": c.RemoveOldBuilds = v == "1"; break;
                    case "release": c.Release = v; break;
                }
            }
            var known = c.GameArg == "--image" || c.GameArg == "--wua" || c.GameArg == "--game-dir";
            return known && c.GamePath != "" ? c : null;
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { return null; }
    }

    public void Save(string work)
    {
        var sb = new StringBuilder();
        void Put(string key, string? value) => sb.Append(key).Append('=').Append(value ?? "").Append('\n');
        Put("game", GamePath);
        Put("format", GameArg);
        Put("forwarder_keys", ForwarderKeys);
        Put("minimap_game", MinimapGame);
        Put("ptbr_download", PtbrDownload);
        Put("prepare_shaders", PrepareShaders ? "1" : "0");
        Put("remove_old_builds", RemoveOldBuilds ? "1" : "0");
        Put("release", Release);
        var path = Path.Combine(work, FileName);
        File.WriteAllText(path + ".part", sb.ToString(), new UTF8Encoding(false));
        if (File.Exists(path)) File.Delete(path);
        File.Move(path + ".part", path);
    }

    // The first saved path that is not there any more (the game in its format, then the optional files), or null.
    // (The pages check the rest, as for a path the player picks.)
    public string? Missing()
    {
        var gameThere = GameArg == "--game-dir"
            ? Directory.Exists(GamePath) && Directory.Exists(Path.Combine(GamePath, "code"))
            : File.Exists(GamePath);
        if (!gameThere) return GamePath;
        foreach (var p in new[] { ForwarderKeys, MinimapGame, PtbrDownload })
            if (p != null && !File.Exists(p) && !Directory.Exists(p)) return p;
        return null;
    }

    // the welcome page's list of these choices, in the pages' own words (their headings without "(optional)")
    public string Describe()
    {
        static string Name(string key) => Regex.Replace(Str.Get(key), @"\s*\([^)]*\)$", "");
        var sb = new StringBuilder();
        var format = GameArg == "--image" ? "game_opt_img" : GameArg == "--wua" ? "game_opt_wua" : "game_opt_dir";
        sb.Append("•  ").Append(Str.Get(format)).Append(": ").Append(GamePath);
        if (ForwarderKeys != null) sb.Append("\n•  ").Append(Name("fwd_h")).Append(": ").Append(ForwarderKeys);
        if (MinimapGame != null) sb.Append("\n•  ").Append(Name("mm_h")).Append(": ").Append(MinimapGame);
        if (PtbrDownload != null) sb.Append("\n•  ").Append(Name("ptbr_h")).Append(": ").Append(PtbrDownload);
        if (PrepareShaders) sb.Append("\n•  ").Append(Name("sh_h"));
        if (RemoveOldBuilds) sb.Append("\n•  ").Append(Name("old_h"));   // (#120: the one-click update removes them too)
        return sb.ToString();
    }

    // vX.Y.Z (and a "-suffix" for a test build): whether `tag` is a later release than `than`. A test build of a
    // version comes before the release itself (v1.0.1-test3 < v1.0.1).
    public static bool IsNewer(string tag, string than)
    {
        static (Version v, bool test)? Parse(string t)
        {
            var m = Regex.Match(t, @"^v?(\d+)\.(\d+)\.(\d+)(-.+)?$");
            if (!m.Success) return null;
            return (new Version(int.Parse(m.Groups[1].Value), int.Parse(m.Groups[2].Value), int.Parse(m.Groups[3].Value)),
                    m.Groups[4].Success);
        }
        var a = Parse(tag);
        var b = Parse(than);
        if (a == null) return false;
        if (b == null) return true;   // (an older installer's build, or none noted: any release is an update)
        var c = a.Value.v.CompareTo(b.Value.v);
        return c > 0 || (c == 0 && b.Value.test && !a.Value.test);
    }
}

abstract partial class EngineBase
{
    // The release an update would build, for the welcome page: a pre-release installer's own, else the latest on
    // GitHub; "" when offline.
    public static string ReleaseToOffer(CancellationToken token) =>
        FixedRelease() is var fixedRel && fixedRel != "" ? fixedRel : FetchLatestRelease(token, null);

    // the end of a successful Run: what the next start offers again
    void SaveChoices()
    {
        try
        {
            new SavedChoices
            {
                GamePath = GamePath, GameArg = GameArg, ForwarderKeys = ForwarderKeys, MinimapGame = MinimapGame,
                PtbrDownload = PtbrDownload, PrepareShaders = PrepareShaders, RemoveOldBuilds = RemoveOldBuilds,
                Release = releaseVer,
            }.Save(Work);
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            Warn(Str.Get("choices_not_saved", e.Message));
        }
    }
}
