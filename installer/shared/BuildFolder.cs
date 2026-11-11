// The work folder: the installer's default, or the one the player picked on the welcome page (#59), remembered for
// the next run so it reuses the build. What a folder needs to work, and the drive it is on. Builds for .NET
// Framework 4.8 (Windows) and .NET 8 (macOS, Linux) alike.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace SwitchWakerHD.Installer;

static class BuildFolder
{
    public const string Name = "SwitchWakerHD-Build";

    static readonly bool Windows = Path.DirectorySeparatorChar == '\\';
    static StringComparison PathCase => Windows ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

    // Two lines: the folder, then the root of the drive it was on (a Mac or Linux mount point, "E:\" on Windows)
    static string SavedFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SwitchWakerHD-Installer", "build-folder.txt");

    // The remembered folder while the drive it is on is there (a card or disk plugged in again), else the default.
    // SavedGone: the remembered folder that is not there now, for the welcome page.
    public static string? SavedGone { get; set; }
    static Func<string>? fallback;
    static string? defaultDir;
    public static string Load(Func<string> fallbackDir)
    {
        fallback = fallbackDir;
        try
        {
            var lines = File.Exists(SavedFile) ? File.ReadAllLines(SavedFile) : new string[0];
            var saved = lines.Length > 0 ? lines[0].Trim() : "";
            if (saved != "")
            {
                if (Present(saved, lines.Length > 1 ? lines[1].Trim() : "")) return saved;
                SavedGone = saved;
            }
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
        return Default;
    }

    // The folder above the remembered one is there, on the drive it was saved on. An empty mount point left by a
    // drive that is not mounted now (/Volumes/X after an unclean eject, /mnt/data) passes the first test but is on
    // the system disk: building there would fill it, hidden once the drive is mounted again.
    static bool Present(string saved, string savedRoot)
    {
        var parent = Path.GetDirectoryName(saved);
        if (parent == null || !Directory.Exists(parent)) return false;
        if (savedRoot == "") return true;
        var root = DriveOf(saved)?.RootDirectory.FullName;
        return root != null && string.Equals(root.TrimEnd('/', '\\'), savedRoot.TrimEnd('/', '\\'), PathCase);
    }

    // The installer's own folder (ChooseWorkDir on Windows, ~/SwitchWakerHD-Build on Mac/Linux), worked out once
    public static string Default => defaultDir ??= fallback?.Invoke() ?? throw new InvalidOperationException("BuildFolder.Load first");
    public static bool IsDefault(string dir) => string.Equals(dir, Default, PathCase);

    // Only a folder the player picked: the default is worked out again on every start (it can change, #30). Back to
    // the default ("Use the default folder", or the default picked in the dialog): the remembered pick goes, once the
    // install starts.
    static bool Picked, Reset;
    public static void Save(string dir)
    {
        try
        {
            if (Reset && IsDefault(dir))
            {
                if (File.Exists(SavedFile)) File.Delete(SavedFile);
                return;
            }
            if (!Picked) return;
            Directory.CreateDirectory(Path.GetDirectoryName(SavedFile)!);
            File.WriteAllText(SavedFile, dir + "\n" + (DriveOf(dir)?.RootDirectory.FullName ?? "") + "\n");
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
    }

    public static string UseDefault()
    {
        Picked = false;
        Reset = true;
        SavedGone = null;
        return Default;
    }

    // The work folder for a folder picked in the dialog: that one when it already is a work folder (IsWorkFolder),
    // else a SwitchWakerHD-Build inside it (picking D:\ builds in D:\SwitchWakerHD-Build).
    public static string ForPick(string picked)
    {
        picked = Path.GetFullPath(picked);
        if (picked.Length > (Path.GetPathRoot(picked)?.Length ?? 0)) picked = picked.TrimEnd('\\', '/');
        var dir = IsWorkFolder(picked) ? picked : Path.Combine(picked, Name);
        var isDefault = IsDefault(dir);
        Picked = !isDefault;
        Reset = isDefault;
        SavedGone = null;
        return dir;
    }

    // A folder the installer made, used as is: by its name, or by what only a build leaves there (its last choices,
    // and its toolchain or Python). Not by an installer.log (other programs write one, players keep copies of ours) or
    // a SwitchWakerHD-v* (a release unzipped by hand in Downloads): the first run replaces python/, and "Remove older
    // builds" deletes releases, in the work folder. Never a drive root or the home folder itself.
    static bool IsWorkFolder(string dir)
    {
        if (dir.Length <= (Path.GetPathRoot(dir)?.Length ?? 0)) return false;
        if (DriveOf(dir) is DriveInfo d && string.Equals(d.RootDirectory.FullName.TrimEnd('/', '\\'), dir, PathCase)) return false;
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile).TrimEnd('\\', '/');
        if (string.Equals(dir, home, PathCase)) return false;
        if (string.Equals(Path.GetFileName(dir), Name, StringComparison.OrdinalIgnoreCase)) return true;
        return File.Exists(Path.Combine(dir, SavedChoices.FileName)) &&
               (Directory.Exists(Path.Combine(dir, "toolchain")) || Directory.Exists(Path.Combine(dir, "python")));
    }

    // The build tools take no spaces or non-ASCII letters (#30); on Windows a drive letter path, within MAX_PATH once
    // the release and its objects are under it (MaxWindowsLength, InstallEngine.ChooseWorkDir).
    public const int MaxWindowsLength = 45;
    public static bool CharsOk(string dir) =>
        Windows ? Regex.IsMatch(dir, @"^[A-Za-z]:\\[A-Za-z0-9_.\\-]*$") : Regex.IsMatch(dir, @"^/[A-Za-z0-9_./-]*$");

    // What the welcome page shows about a folder. Problem: why the build cannot use it, or null; it blocks the
    // install for a folder the player picked, while the installer's own folder is only warned about (Blocking).
    public sealed class State
    {
        public string Dir = "";
        public bool IsDefault;
        public string? Problem;
        public string? Blocking => IsDefault ? null : Problem;
        public string? Warn;     // the problem, or the remembered folder that is not there
        public string Space = "";
        public bool Low;         // not enough free space there
        public string Notes = "";
    }

    // systemCheck: what the system adds, tried in the folder (Mac/Linux: symbolic links and programs on that drive):
    // a string key for what the drive cannot do, or null. The folder is tried again only on recheck or for another
    // folder: not on every language switch or page change.
    public static State Describe(string dir, Func<string, string?>? systemCheck = null, bool recheck = false)
    {
        var s = new State { Dir = dir, IsDefault = IsDefault(dir) };
        var drive = DriveOf(dir);
        s.Problem = Problem(dir, drive, systemCheck, recheck);
        s.Space = SpaceLine(dir, drive, out s.Low);
        s.Warn = s.Problem ?? (SavedGone != null ? Str.Get("dir_saved_gone", SavedGone) : null);
        var notes = new List<string>();
        if (s.Problem == null && FatFormat(drive) is string fat) notes.Add(Str.Get("dir_fat", fat));
        if (s.Problem == null && IsRemovable(drive)) notes.Add(Str.Get("dir_removable", NeedGb(dir) + 2));
        s.Notes = string.Join("\n\n", notes);
        return s;
    }

    // No network drive (a mapped letter, an SMB or NFS mount): hundreds of thousands of small files make the build
    // crawl or fail there.
    static string? Problem(string dir, DriveInfo? drive, Func<string, string?>? systemCheck, bool recheck)
    {
        if (!CharsOk(dir)) return Str.Get("dir_bad_chars", dir);
        if (Windows && dir.Length > MaxWindowsLength) return Str.Get("dir_too_long", dir, MaxWindowsLength);
        if (drive?.DriveType == DriveType.Network) return Str.Get("dir_network", dir);
        if (recheck || probedDir != dir)
        {
            probed = Probe(dir, systemCheck);
            probedDir = dir;
        }
        return probed.key == null ? null : Str.Get(probed.key, dir, probed.arg ?? "");
    }

    static string? probedDir;
    static (string? key, string? arg) probed;

    // Tried in the folder itself; a folder made only for the test goes again (the install makes it)
    static (string? key, string? arg) Probe(string dir, Func<string, string?>? systemCheck)
    {
        var existed = Directory.Exists(dir);
        try
        {
            Directory.CreateDirectory(dir);
            var probe = Path.Combine(dir, ".write-test");
            File.WriteAllText(probe, "");
            File.Delete(probe);
            return (systemCheck?.Invoke(dir), null);
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException || e is NotSupportedException)
        {
            return ("dir_no_write", e.Message);
        }
        finally
        {
            try { if (!existed && Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir); }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
        }
    }

    // What the build needs there: the extracted game twice (build/sd-game and the SD folder), the generated code, the
    // objects: ~9 GB the first time; a re-run reuses most of it.
    public static int NeedGb(string dir)
    {
        try { return Directory.Exists(dir) && Directory.GetDirectories(dir, "SwitchWakerHD-v*").Length > 0 ? 5 : 10; }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { return 10; }
    }

    // The drive the folder is on: on Mac/Linux the mount point with the longest matching path, not "/". Not only
    // the ready ones: a network mount that does not answer is still a network drive (its free space then throws).
    public static DriveInfo? DriveOf(string dir)
    {
        try
        {
            if (Windows) return new DriveInfo(Path.GetPathRoot(dir)!);
            return DriveInfo.GetDrives()
                .Where(d => dir == d.RootDirectory.FullName ||
                            dir.StartsWith(d.RootDirectory.FullName.TrimEnd('/') + "/", StringComparison.Ordinal))
                .OrderByDescending(d => d.RootDirectory.FullName.Length)
                .FirstOrDefault();
        }
        catch (Exception e) when (e is ArgumentException || e is IOException || e is UnauthorizedAccessException) { return null; }
    }

    // A card or USB drive: a slower build, and it has to stay connected. On Mac/Linux a disk the system mounted for
    // the user (macOS /Volumes, udisks /media and /run/media); not /mnt, where internal disks go.
    static bool IsRemovable(DriveInfo? drive)
    {
        if (drive == null) return false;
        try
        {
            if (Windows) return drive.DriveType == DriveType.Removable;
            var root = drive.RootDirectory.FullName;
            return new[] { "/Volumes/", "/media/", "/run/media/" }.Any(p => root.StartsWith(p, StringComparison.Ordinal));
        }
        catch (IOException) { return false; }
    }

    // FAT32 or exFAT (an SD card, most USB sticks): the build works there (Windows copies the toolchain's links,
    // macOS keeps them; Linux FAT fails DesktopEngine.LinksAndPrograms first), only more slowly. Its name, or null.
    static string? FatFormat(DriveInfo? drive)
    {
        try
        {
            var f = drive?.DriveFormat ?? "";
            if (f.Equals("FAT32", StringComparison.OrdinalIgnoreCase) || f.Equals("msdos", StringComparison.OrdinalIgnoreCase) ||
                f.Equals("vfat", StringComparison.OrdinalIgnoreCase) || f.Equals("FAT", StringComparison.OrdinalIgnoreCase))
                return "FAT32";
            if (f.Equals("exFAT", StringComparison.OrdinalIgnoreCase)) return "exFAT";
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
        return null;
    }

    // The welcome page's line on the folder's drive: free space against what the build needs (low: not enough).
    static string SpaceLine(string dir, DriveInfo? drive, out bool low)
    {
        low = false;
        if (drive == null) return "";
        try
        {
            var freeGb = drive.AvailableFreeSpace / 1e9;
            var need = NeedGb(dir);
            low = freeGb < need;
            return Str.Get(low ? "dir_space_low" : "dir_space", drive.Name, freeGb.ToString("0.0"), need);
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { return ""; }
    }
}
