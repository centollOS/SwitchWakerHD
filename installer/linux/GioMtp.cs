// Copies the SD folder to a Switch running DBI's MTP responder ("Run MTP responder", USB cable), through gio
// (GLib/GVFS's MTP backend, which GNOME, Cinnamon, MATE, Xfce and most desktops have): the same path the file
// manager takes. File by file, to report progress and to skip what is already there with the same size (a
// copy that was cut off resumes where it stopped).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;

namespace SwitchWakerHD.Installer;

sealed class MtpStorage
{
    public string Device = "";
    public string Name = "";
    public string Uri = "";
}

static class GioMtp
{
    static readonly Regex MountLine = new(@"^\s*Mount\(\d+\):\s*(.*?)\s*->\s*(mtp://\S+)", RegexOptions.Multiline);
    static readonly Regex ActivationRoot = new(@"activation_root=(mtp://\S+)");

    public static bool Available => Run("gio", "version").code == 0;

    // The SD card storages of the connected MTP devices. DBI calls it "SD Card" or "External SD Card";
    // "SD Card install" is its NSP installer, not the card. Devices that are plugged in but not mounted yet
    // are mounted first (desktops usually do it themselves).
    public static List<MtpStorage> FindSdStorages()
    {
        var found = new List<MtpStorage>();
        if (!Available) return found;
        var (_, list) = Run("gio", "mount -li");
        var mounted = MountLine.Matches(list).Cast<Match>().Select(m => m.Groups[2].Value.TrimEnd('/')).ToHashSet();
        foreach (Match m in ActivationRoot.Matches(list))
            if (!mounted.Contains(m.Groups[1].Value.TrimEnd('/')))
                Run("gio", $"mount \"{m.Groups[1].Value}\"", TimeSpan.FromSeconds(20));

        (_, list) = Run("gio", "mount -l");
        foreach (Match m in MountLine.Matches(list))
        {
            var device = m.Groups[1].Value;
            var root = m.Groups[2].Value.TrimEnd('/');
            foreach (var (name, display) in Children(root))
            {
                if (display.IndexOf("SD Card", StringComparison.OrdinalIgnoreCase) >= 0 &&
                    display.IndexOf("install", StringComparison.OrdinalIgnoreCase) < 0)
                    found.Add(new MtpStorage { Device = device, Name = display, Uri = root + "/" + Uri.EscapeDataString(name) });
            }
        }
        return found;
    }

    // (name, display name) of a folder's entries: "NAME\tSIZE\t(type)\tstandard::display-name=DISPLAY"
    static IEnumerable<(string name, string display)> Children(string uri)
    {
        var (code, output) = Run("gio", $"list -a standard::display-name \"{uri}\"", TimeSpan.FromSeconds(30));
        if (code != 0) yield break;
        foreach (var line in output.Split('\n'))
        {
            var cols = line.Split('\t');
            if (cols.Length < 1 || cols[0].Length == 0) continue;
            var attr = cols.FirstOrDefault(c => c.StartsWith("standard::display-name="));
            yield return (cols[0], attr != null ? attr.Substring("standard::display-name=".Length) : cols[0]);
        }
    }

    // progress(filesDone, filesTotal, bytesDone, bytesTotal, current file)
    public static void Copy(string srcDir, MtpStorage target, Action<int, int, long, long, string> progress,
                            Action<string> log, CancellationToken ct)
    {
        if (Run("gio", $"info \"{target.Uri}\"", TimeSpan.FromSeconds(30)).code != 0)
            throw new InstallException(Str.Get("mtp_lost"));

        var files = Directory.GetFiles(srcDir, "*", SearchOption.AllDirectories);
        long bytesTotal = files.Sum(f => new FileInfo(f).Length), bytesDone = 0;
        int filesDone = 0;
        var made = new HashSet<string>();
        foreach (var file in files)
        {
            ct.ThrowIfCancellationRequested();
            var rel = file.Substring(srcDir.Length).TrimStart('/');
            var len = new FileInfo(file).Length;
            progress(filesDone, files.Length, bytesDone, bytesTotal, rel);

            var dirRel = Path.GetDirectoryName(rel) ?? "";
            if (dirRel.Length > 0 && made.Add(dirRel))
            {
                var r = Run("gio", $"mkdir -p \"{UriOf(target, dirRel)}\"", TimeSpan.FromSeconds(60));
                if (r.code != 0 && Run("gio", $"info \"{UriOf(target, dirRel)}\"", TimeSpan.FromSeconds(30)).code != 0)
                {
                    log(r.output.Trim());
                    throw new InstallException(Str.Get("mtp_mkdir_fail", dirRel));
                }
            }

            var dst = UriOf(target, rel);
            if (SizeOf(dst) != len)
            {
                // gio copy replaces an existing file (only -i would ask)
                var r = Run("gio", $"copy \"{file}\" \"{dst}\"", TimeSpan.FromSeconds(120 + len / (1024 * 1024)));
                if (r.code != 0 || SizeOf(dst) != len)
                {
                    log(r.output.Trim());
                    throw new InstallException(Str.Get("mtp_copy_fail", rel));
                }
            }
            filesDone++;
            bytesDone += len;
        }
        progress(filesDone, files.Length, bytesTotal, bytesTotal, "");
    }

    static string UriOf(MtpStorage target, string rel) =>
        target.Uri + "/" + string.Join("/", rel.Split('/').Select(Uri.EscapeDataString));

    static long SizeOf(string uri)
    {
        var (code, output) = Run("gio", $"info -a standard::size \"{uri}\"", TimeSpan.FromSeconds(30));
        if (code != 0) return -1;
        var m = Regex.Match(output, @"standard::size:\s*(\d+)");
        return m.Success ? long.Parse(m.Groups[1].Value) : -1;
    }

    static (int code, string output) Run(string exe, string args, TimeSpan? timeout = null)
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
            if (!proc.WaitForExit((int)(timeout ?? TimeSpan.FromSeconds(30)).TotalMilliseconds))
            {
                try { proc.Kill(true); } catch { }
                return (-1, "timeout");
            }
            return (proc.ExitCode, stdout.Result + stderr.Result);
        }
        catch (Exception e) { return (-1, e.Message); }
    }
}
