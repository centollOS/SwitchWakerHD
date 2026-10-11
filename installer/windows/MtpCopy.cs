// Copies the SD folder to a Switch running DBI's MTP responder ("Run MTP responder", USB cable), through the
// Windows shell: the same path Explorer takes, so no driver or library is needed. File by file, to report
// progress and to skip what is already there (a copy that was cut off resumes where it stopped).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace SwitchWakerHD.Installer;

sealed class MtpStorage
{
    public string Device = "";
    public string Name = "";
}

static class MtpCopy
{
    // Shell COM objects need an STA thread; each call runs on its own.
    public static Task<T> Sta<T>(Func<T> body)
    {
        var tcs = new TaskCompletionSource<T>();
        var t = new Thread(() =>
        {
            try { tcs.SetResult(body()); }
            catch (Exception e) { tcs.SetException(e); }
        });
        t.SetApartmentState(ApartmentState.STA);
        t.IsBackground = true;
        t.Start();
        return tcs.Task;
    }

    static dynamic Shell() => Activator.CreateInstance(Type.GetTypeFromProgID("Shell.Application")!)!;

    // The SD card storages of the connected portable devices. DBI calls it "SD Card" or "External SD Card";
    // "SD Card install" is its NSP installer, not the card.
    public static List<MtpStorage> FindSdStorages()
    {
        var found = new List<MtpStorage>();
        foreach (var (device, storage) in Storages())
        {
            string name = storage.Name;
            if (name.IndexOf("SD Card", StringComparison.OrdinalIgnoreCase) >= 0 &&
                name.IndexOf("install", StringComparison.OrdinalIgnoreCase) < 0)
                found.Add(new MtpStorage { Device = device, Name = name });
        }
        return found;
    }

    static IEnumerable<(string device, dynamic storage)> Storages()
    {
        dynamic pc = Shell().NameSpace(17);   // This PC
        foreach (dynamic dev in pc.Items())
        {
            bool fileSystem;
            try { fileSystem = dev.IsFileSystem; } catch { continue; }
            if (fileSystem) continue;          // drive letters and ordinary folders
            dynamic? folder = null;
            try { folder = dev.GetFolder; } catch { }
            if (folder == null) continue;
            string devName = dev.Name;
            foreach (dynamic st in folder.Items()) yield return (devName, st);
        }
    }

    // progress(filesDone, filesTotal, bytesDone, bytesTotal, current file)
    public static void Copy(string srcDir, MtpStorage target, Action<int, int, long, long, string> progress, CancellationToken ct)
    {
        dynamic? root = null;
        foreach (var (device, storage) in Storages())
            if (device == target.Device && (string)storage.Name == target.Name) { root = storage.GetFolder; break; }
        if (root == null) throw new InstallException(Str.Get("mtp_lost"));

        var files = Directory.GetFiles(srcDir, "*", SearchOption.AllDirectories);
        var state = new CopyState
        {
            FilesTotal = files.Length,
            BytesTotal = files.Sum(f => new FileInfo(f).Length),
            Progress = progress,
            Ct = ct,
        };
        CopyDir(srcDir, root, state);
        progress(state.FilesDone, state.FilesTotal, state.BytesTotal, state.BytesTotal, "");
    }

    sealed class CopyState
    {
        public int FilesDone, FilesTotal;
        public long BytesDone, BytesTotal;
        public Action<int, int, long, long, string> Progress = delegate { };
        public CancellationToken Ct;
    }

    // CopyHere flags: no progress window, yes to all, no "create folder?" question, no error dialog.
    // (Windows ignores some of them for portable devices: a "replace?" question can still appear.)
    const int CopyFlags = 4 | 16 | 512 | 1024;

    static void CopyDir(string src, dynamic dst, CopyState s)
    {
        foreach (var dir in Directory.GetDirectories(src))
        {
            var name = Path.GetFileName(dir);
            dynamic? item = Child(dst, name);
            if (item == null)
            {
                dst.NewFolder(name);
                item = WaitFor(() => Child(dst, name), TimeSpan.FromSeconds(30), s.Ct)
                       ?? throw new InstallException(Str.Get("mtp_mkdir_fail", name));
            }
            CopyDir(dir, item.GetFolder, s);
        }

        foreach (var file in Directory.GetFiles(src))
        {
            s.Ct.ThrowIfCancellationRequested();
            var name = Path.GetFileName(file);
            var len = new FileInfo(file).Length;
            s.Progress(s.FilesDone, s.FilesTotal, s.BytesDone, s.BytesTotal, name);

            dynamic? have = Child(dst, name);
            if (have == null || SizeOf(have) != len)
            {
                dst.CopyHere(file, CopyFlags);
                // Done once the file is there with its full size.
                var timeout = TimeSpan.FromSeconds(120 + len / (1024 * 1024));
                var ok = WaitFor(() =>
                {
                    var i = Child(dst, name);
                    return i != null && SizeOf(i) == len ? i : null;
                }, timeout, s.Ct);
                if (ok == null) throw new InstallException(Str.Get("mtp_copy_fail", name));
            }
            s.FilesDone++;
            s.BytesDone += len;
        }
    }

    static long SizeOf(dynamic item)
    {
        try { return (long)item.Size; } catch { return -1; }
    }

    // A child by name: ParseName, else a scan (some portable devices only answer one of the two).
    static dynamic? Child(dynamic folder, string name)
    {
        try
        {
            dynamic? i = folder.ParseName(name);
            if (i != null) return i;
        }
        catch { }
        var bare = Path.GetFileNameWithoutExtension(name);
        dynamic? loose = null;
        foreach (dynamic i in folder.Items())
        {
            string n = i.Name;
            if (string.Equals(n, name, StringComparison.OrdinalIgnoreCase)) return i;
            // Explorer hides known extensions in display names.
            if (loose == null && string.Equals(n, bare, StringComparison.OrdinalIgnoreCase)) loose = i;
        }
        return loose;
    }

    static dynamic? WaitFor(Func<dynamic?> probe, TimeSpan timeout, CancellationToken ct)
    {
        var end = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < end)
        {
            ct.ThrowIfCancellationRequested();
            var r = probe();
            if (r != null) return r;
            Thread.Sleep(300);
        }
        return null;
    }
}
