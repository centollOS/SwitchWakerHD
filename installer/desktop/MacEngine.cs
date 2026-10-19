// The macOS install steps (the shared ones are in DesktopEngine.cs and shared/EngineBase.cs): Apple's Command Line
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

sealed class MacEngine : DesktopEngine
{
    // devkitPro's macOS installer: pacman for arm64 and x86_64, the dkp-libs and dkp-osx repositories
    const string DkpPkgUrl = "https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg";
    const string DkpPkgSha = "da74156211cd3d8664ba7fb95544fbc0a2fbef1dd87b1a6bfafb30d2d446cd30";
    // run through devkitPro's wrapper: it puts its gpg on PATH (the bare pacman fails with "Invalid crypto engine")
    const string DkpPacman = "/usr/local/bin/dkp-pacman";

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
        InstallDevkit(pkg);
    }

    // The root script: devkitPro's pkg when given, then the Switch packages; fails with the reason when a package
    // is still missing after it.
    void InstallDevkit(string? pkg)
    {
        var log = Path.Combine(WorkDir, "devkitpro-root.log");
        var script = Path.Combine(WorkDir, "devkitpro-root.sh");
        var sb = new StringBuilder();
        sb.Append("#!/bin/sh\n# SwitchWakerHD installer: devkitPro and its Switch packages (run once as root)\ncd /\n");
        sb.Append($"exec > {ShQuote(log)} 2>&1\n");
        if (pkg != null) sb.Append($"/usr/sbin/installer -pkg {ShQuote(pkg)} -target / || exit 10\n");
        // pkg.devkitpro.org may answer 403 (as on Windows, #28): then again with the downloads through our Python,
        // and last from the package lists already there
        sb.Append(PacmanXferFunction(DkpPacman));
        sb.Append($"{DkpPacman} -Sy --needed --noconfirm {DkpPackages} && exit 0\n");
        sb.Append("dkp_xfer && exit 0\n");
        sb.Append($"echo '== retry without refreshing the package lists'\n{DkpPacman} -S --needed --noconfirm {DkpPackages}\n");
        File.WriteAllText(script, sb.ToString());
        File.Delete(log);

        Say(Str.Get("dkp_mac_password"));
        // The AppleScript goes in a file: on a command line its quotes would not survive
        var prompt = Str.Get("dkp_mac_prompt").Replace("\\", "\\\\").Replace("\"", "\\\"");
        var apple = Path.Combine(WorkDir, "devkitpro-root.applescript");
        File.WriteAllText(apple, $"do shell script \"/bin/sh '{script.Replace("'", "'\\\\''")}'\" " +
                                 $"with administrator privileges with prompt \"{prompt}\"\n");
        var output = new List<string>();
        int rc;
        using (var follow = FollowLog(log, output))
            rc = Exec("/usr/bin/osascript", Quote(apple), check: false, onLine: output.Add);
        if (rc != 0 && output.Any(l => l.Contains("(-128)")))
            throw new InstallException(Str.Get("dkp_mac_cancelled"));
        // osascript failed before the script ran (no log): say so, whatever is installed already
        if (rc != 0 && !File.Exists(log))
            throw new InstallException(Str.Get("dkp_mac_fail", string.Join(" ", output.TakeLast(3))));

        var missing = MissingDevkit();
        if (missing.Count > 0)
        {
            var why = PacmanFailure(output);
            throw new InstallException(Str.Get("dkp_missing", string.Join(", ", missing)) + " " + Str.Get(why));
        }
        if (rc != 0) Warn(Str.Get("dkp_pkg_warn") + " " + Str.Get(PacmanFailure(output)));
        Ok(Str.Get("dkp_tools_ok"));
    }

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

    // Mounted volumes that can be the Switch's card: under /Volumes, ejectable (a card reader, hekate's UMS) or
    // holding the Switch's folders; never the Mac's own disk.
    public override List<Volume> SdVolumes()
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

    public override bool Eject(string mount) => Capture("/usr/sbin/diskutil", "eject " + Quote(mount)).code == 0;

    public override void Open(string path, bool text = false)
    {
        try
        {
            var psi = new ProcessStartInfo("/usr/bin/open") { UseShellExecute = false };
            if (text) { psi.ArgumentList.Add("-a"); psi.ArgumentList.Add("TextEdit"); }
            psi.ArgumentList.Add(path);
            Process.Start(psi)?.Dispose();
        }
        catch (Exception) { }
    }
}
