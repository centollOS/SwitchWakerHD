// The work folder: the installer's default, or the one the player picked on the welcome page (#59), remembered for
// the next run so it reuses the build. What a folder needs to work, and the drive it is on. Builds for .NET
// Framework 4.8 (Windows) and .NET 8 (macOS, Linux) alike.
using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace SwitchWakerHD.Installer;

static class BuildFolder
{
    public const string Name = "SwitchWakerHD-Build";

    static readonly bool Windows = Path.DirectorySeparatorChar == '\\';

    static string SavedFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SwitchWakerHD-Installer", "build-folder.txt");

    // The remembered folder while the drive it is on is there (a card or disk plugged in again), else the default.
    // SavedGone: the remembered folder that is not there now, for the welcome page.
    public static string? SavedGone { get; set; }
    public static string Load(Func<string> fallback)
    {
        try
        {
            var saved = File.Exists(SavedFile) ? File.ReadAllText(SavedFile).Trim() : "";
            if (saved != "")
            {
                var parent = Path.GetDirectoryName(saved);
                if (parent != null && Directory.Exists(parent)) return saved;
                SavedGone = saved;
            }
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
        return fallback();
    }

    // Only a folder the player picked: the default is worked out again on every start (it can change, #30)
    static bool Picked;
    public static void Save(string dir)
    {
        if (!Picked) return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SavedFile)!);
            File.WriteAllText(SavedFile, dir);
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
    }

    // The work folder for a folder picked in the dialog: that one when it already is a work folder (by its name or
    // its log), else a SwitchWakerHD-Build inside it (picking D:\ builds in D:\SwitchWakerHD-Build). Not by a
    // SwitchWakerHD-v* inside: Downloads with the release unzipped by hand is no work folder.
    public static string ForPick(string picked)
    {
        Picked = true;
        picked = Path.GetFullPath(picked);
        if (picked.Length > (Path.GetPathRoot(picked)?.Length ?? 0)) picked = picked.TrimEnd('\\', '/');
        if (string.Equals(Path.GetFileName(picked), Name, StringComparison.OrdinalIgnoreCase) ||
            File.Exists(Path.Combine(picked, "installer.log")))
            return picked;
        return Path.Combine(picked, Name);
    }

    // Why the build cannot use this folder, or null. The build tools take no spaces or non-ASCII letters (#30);
    // on Windows a drive letter path within MAX_PATH once the release and its objects are under it (InstallEngine).
    // systemCheck: what the system adds (Mac/Linux: symbolic links and programs on that drive).
    public const int MaxWindowsLength = 45;
    public static string? Problem(string dir, Func<string, string?>? systemCheck = null)
    {
        if (Windows ? !Regex.IsMatch(dir, @"^[A-Za-z]:\\[A-Za-z0-9_.\\-]*$") : !Regex.IsMatch(dir, @"^/[A-Za-z0-9_./-]*$"))
            return Str.Get("dir_bad_chars", dir);
        if (Windows && dir.Length > MaxWindowsLength) return Str.Get("dir_too_long", dir, MaxWindowsLength);
        // tried in the folder itself; a folder made only for the test goes again (the install makes it)
        var existed = Directory.Exists(dir);
        try
        {
            Directory.CreateDirectory(dir);
            var probe = Path.Combine(dir, ".write-test");
            File.WriteAllText(probe, "");
            File.Delete(probe);
            return systemCheck?.Invoke(dir);
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is NotSupportedException)
        {
            return Str.Get("dir_no_write", dir, e.Message);
        }
        finally
        {
            try { if (!existed && Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir); }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
        }
    }

    // What the build needs there: the extracted game twice (build/sd-game and the SD folder), the generated code, the
    // objects: ~9 GB the first time; a re-run reuses most of it.
    public static int NeedGb(string dir) =>
        Directory.Exists(dir) && Directory.GetDirectories(dir, "SwitchWakerHD-v*").Length > 0 ? 5 : 10;

    // The drive the folder is on: on Mac/Linux the mount point with the longest matching path, not "/".
    public static DriveInfo? DriveOf(string dir)
    {
        try
        {
            if (Windows) return new DriveInfo(Path.GetPathRoot(dir)!);
            return DriveInfo.GetDrives()
                .Where(d => d.IsReady && (dir == d.RootDirectory.FullName ||
                                          dir.StartsWith(d.RootDirectory.FullName.TrimEnd('/') + "/", StringComparison.Ordinal)))
                .OrderByDescending(d => d.RootDirectory.FullName.Length)
                .FirstOrDefault();
        }
        catch (Exception e) when (e is ArgumentException || e is IOException || e is UnauthorizedAccessException) { return null; }
    }

    // A card or USB drive: a slower build, and it has to stay connected
    public static bool IsRemovable(string dir)
    {
        if (!Windows) return new[] { "/Volumes/", "/media/", "/run/media/", "/mnt/" }.Any(p => dir.StartsWith(p, StringComparison.Ordinal));
        try { return DriveOf(dir)?.DriveType == DriveType.Removable; }
        catch (IOException) { return false; }
    }

    // The welcome page's line on the folder's drive: free space against what the build needs (low: not enough).
    public static string SpaceLine(string dir, out bool low)
    {
        low = false;
        var drive = DriveOf(dir);
        if (drive == null) return "";
        try
        {
            var freeGb = drive.AvailableFreeSpace / 1e9;
            var need = NeedGb(dir);
            low = freeGb < need;
            return Str.Get(low ? "dir_space_low" : "dir_space", drive.Name, freeGb.ToString("0.0"), need);
        }
        catch (IOException) { return ""; }
    }
}
