// "Remove older builds" (#120): each release builds in its own folder of the work folder (SwitchWakerHD-vX.Y.Z, 5-10
// GB with its zip, the extracted game twice, the generated code and the objects), and nothing removed the earlier
// ones. After a successful build, with the option on, the installer removes what it made itself for releases older
// than the one just built: their folders, zips and shader templates, and the pinned toolchains no release in the
// work folder uses any more. Nothing else: the player's choices, the log, the download cache, a newer release's
// files (a test installer building an older pre-release) and anything it does not recognise stay.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace SwitchWakerHD.Installer;

abstract partial class EngineBase
{
    public bool RemoveOldBuilds = true;

    // the work folder's entries the installer makes for a release, with the release's tag (vX.Y.Z or vX.Y.Z-suffix)
    static readonly Regex ReleaseEntry = new(
        @"^(?:SwitchWakerHD-(v\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+?)?)(?:\.tmp|\.zip|\.zip\.part|\.zip\.sha256)?" +
        @"|shader-template-(v\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+?)?)\.sealed(?:\.part|\.sha256)?)$");

    // What RemoveOlderBuilds would remove: entries of Work (full paths) for releases older than `current`.
    internal static List<string> OlderBuildEntries(string work, string current)
    {
        var found = new List<string>();
        if (current == "" || !Directory.Exists(work)) return found;
        foreach (var path in Directory.GetFileSystemEntries(work))
        {
            var m = ReleaseEntry.Match(Path.GetFileName(path));
            if (!m.Success) continue;
            var tag = m.Groups[1].Success ? m.Groups[1].Value : m.Groups[2].Value;
            if (SavedChoices.IsNewer(current, tag)) found.Add(path);
        }
        found.Sort(StringComparer.Ordinal);
        return found;
    }

    // The pinned toolchains (Work/toolchain/<id>, ~1 GB each) other than the one this build used. (A newer release
    // left in Work, if any, fetches its own again from the download cache when it next runs.)
    List<string> UnusedToolchains()
    {
        var unused = new List<string>();
        if (pinnedToolchain == null) return unused;   // a release without one: nothing of ours to judge
        var inUse = Path.GetFullPath(Path.GetDirectoryName(pinnedToolchain)!);
        if (!Directory.Exists(ToolchainDir)) return unused;
        foreach (var dir in Directory.GetDirectories(ToolchainDir))
        {
            // the ids are 12 hex digits (fetch.py's lock_id)
            if (!Regex.IsMatch(Path.GetFileName(dir), "^[0-9a-f]{12}$")) continue;
            if (!string.Equals(Path.GetFullPath(dir), inUse, StringComparison.OrdinalIgnoreCase)) unused.Add(dir);
        }
        return unused;
    }

    // The end of a successful Run, with RemoveOldBuilds: never a reason to fail the install.
    void RemoveOlderBuilds()
    {
        if (!RemoveOldBuilds) return;
        List<string> entries;
        try
        {
            entries = OlderBuildEntries(Work, releaseVer);
            // the toolchains only together with older releases (the one in use is kept)
            if (entries.Count > 0) entries.AddRange(UnusedToolchains());
        }
        catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
        {
            Warn(Str.Get("old_fail", Work, e.Message));
            return;
        }
        if (entries.Count == 0) return;
        Say(Str.Get("old_removing"));
        long before = FreeBytes();
        var removed = new List<string>();
        foreach (var path in entries)
        {
            ct.ThrowIfCancellationRequested();
            try
            {
                DeleteEntry(path);
                removed.Add(Path.GetFileName(path));
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
            {
                Warn(Str.Get("old_fail", path, e.Message));
            }
        }
        if (removed.Count == 0) return;
        long after = FreeBytes();
        var freed = before >= 0 && after > before ? (after - before) / 1e9 : 0;
        Ok(Str.Get("old_removed", string.Join(", ", removed.Where(n => !Regex.IsMatch(n, "^[0-9a-f]{12}$"))
                                                         .DefaultIfEmpty(Str.Get("old_toolchains"))),
                   freed.ToString("0.0")));
    }

    long FreeBytes()
    {
        try { return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(Work))!).AvailableFreeSpace; }
        catch (Exception e) when (e is ArgumentException || e is IOException || e is UnauthorizedAccessException) { return -1; }
    }

    // A file, or a folder with everything in it. A link (a junction or symbolic link) is removed itself, never what it
    // points to. Read-only files (unpacked from an archive) are made writable first.
    static void DeleteEntry(string path)
    {
        var attr = File.GetAttributes(path);
        if ((attr & FileAttributes.ReparsePoint) != 0)
        {
            if ((attr & FileAttributes.Directory) != 0) Directory.Delete(path, false);
            else File.Delete(path);
            return;
        }
        if ((attr & FileAttributes.Directory) == 0)
        {
            if ((attr & FileAttributes.ReadOnly) != 0) File.SetAttributes(path, attr & ~FileAttributes.ReadOnly);
            File.Delete(path);
            return;
        }
        foreach (var child in Directory.GetFileSystemEntries(path)) DeleteEntry(child);
        if ((attr & FileAttributes.ReadOnly) != 0) File.SetAttributes(path, attr & ~FileAttributes.ReadOnly);
        Directory.Delete(path, false);
    }
}
