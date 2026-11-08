// The Linux install steps (the shared ones are in DesktopEngine.cs and shared/EngineBase.cs), from PR #16: devkitPro
// already installed -> a native build; on Debian/Ubuntu without it, devkitPro's own apt route (needed for the
// HOME-screen icon, which builds natively only); else a Docker or Podman the user may use (make_sd.py builds in the
// devkitpro/devkita64 image); else Podman from the distribution. Root steps go through pkexec when the desktop can
// ask for the password, else the user runs one command in a terminal; as root (a container, sudo) they run directly.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace SwitchWakerHD.Installer;

sealed class LinuxEngine : DesktopEngine
{
    const string DkpImage = "docker.io/devkitpro/devkita64:latest";

    string? containerEngine;   // docker or podman, when the build runs in a container

    [DllImport("libc")]
    static extern uint geteuid();

    [DllImport("libc")]
    static extern int posix_fadvise(int fd, long offset, long len, int advice);
    const int POSIX_FADV_DONTNEED = 4;

    // drops the file's clean pages: the next read comes from the card (written ones are clean after the flush)
    protected override void Uncache(FileStream file) =>
        posix_fadvise((int)file.SafeFileHandle.DangerousGetHandle(), 0, 0, POSIX_FADV_DONTNEED);

    // ── Switch toolchain ─────────────────────────────────────────────────────
    protected override void Toolchain()
    {
        Say(Str.Get("dkp_searching"));
        var pm = PackageManager();
        for (int attempt = 0; ; attempt++)
        {
            if (NativeReady()) return;
            // the HOME-screen icon builds only natively: on Debian/Ubuntu install devkitPro for it, even with Docker
            var wantNative = pm == "apt-get" && (ForwarderKeys != null || FindContainer(quiet: true) == null);
            if (attempt == 0 && wantNative)
            {
                RunAsRoot(Str.Get("lx_tc_what_dkp"), DevkitProAptScript());
                continue;
            }
            if (UseContainer()) return;
            if (attempt > 0) throw new InstallException(Str.Get("lx_tc_not_found"));
            if (pm is "dnf" or "pacman" or "zypper")
                RunAsRoot(Str.Get("lx_tc_what_podman"), PackageScript(pm, new Dictionary<string, string>
                {
                    ["dnf"] = "dnf install -y podman",
                    ["pacman"] = "pacman -S --needed --noconfirm podman",
                    ["zypper"] = "zypper --non-interactive install podman",
                }));
            else if (pm != "apt-get")
                AskUser?.Invoke(Str.Get("lx_root_h"), Str.Get("lx_tc_unknown"), Str.Get("btn_done_continue"));
        }
    }

    // devkitPro under /opt/devkitpro with the Switch packages, plus make and a C compiler (and git for the icon):
    // the native build.
    bool NativeReady()
    {
        var missing = MissingDevkit();
        if (missing.Count == DkpFilesCount) return false;   // no devkitPro at all
        if (missing.Count > 0) { Warn(Str.Get("lx_tc_pkgs", string.Join(" ", missing))); return false; }
        var tools = new List<string> { "make", "cc" };
        if (ForwarderKeys != null) tools.Add("git");
        var hostMissing = tools.Where(t => FindOnPath(t) == null).ToList();
        if (hostMissing.Count > 0) { Warn(Str.Get("lx_tc_hosttools", string.Join(" ", hostMissing))); return false; }
        containerEngine = null;
        Ok(Str.Get("lx_tc_native", Dkp));
        return true;
    }

    // build.sh picks podman first, then docker; installed is not enough, the user must be allowed to use it
    // (docker's socket wants the docker group).
    string? FindContainer(bool quiet)
    {
        foreach (var engine in new[] { "podman", "docker" })
        {
            if (FindOnPath(engine) == null) continue;
            var (code, output) = Capture(engine, "info");
            if (code == 0) return engine;
            if (!quiet)
            {
                Warn(Str.Get("lx_tc_docker_denied", engine));
                OnLog(output.Trim(), LogKind.Output);
            }
        }
        return null;
    }

    bool UseContainer()
    {
        containerEngine = FindContainer(quiet: false);
        if (containerEngine == null) return false;
        Ok(Str.Get("lx_tc_container", containerEngine));
        if (ForwarderKeys != null) Warn(Str.Get("lx_fwd_container"));
        return true;
    }

    // devkitPro's own apt route (https://devkitpro.org/wiki/devkitPro_pacman), its install-devkitpro-pacman steps
    // done here: the key, the repository, devkitpro-pacman; then the Switch packages, and what the native build and
    // the icon run (cmake comes from pip). devkitPro's servers sit behind Cloudflare's bot rules, which refuse the
    // download tools' own user agents (curl, Wget, Python-urllib: 403) but let package managers' through (apt,
    // pacman) and the custom one their script uses for the key, "dkp apt": so the key is fetched exactly as theirs
    // does, and the rest goes through apt and dkp-pacman (docs/linux-installer-plan.md, "devkitPro's 403").
    string DevkitProAptScript()
    {
        const string key = "/usr/share/keyring/devkitpro-pub.gpg", list = "/etc/apt/sources.list.d/devkitpro.list";
        var sb = new StringBuilder();
        sb.AppendLine("set -e");
        sb.AppendLine("export DEBIAN_FRONTEND=noninteractive");
        sb.AppendLine("apt-get update");
        // g++ and zlib1g-dev: the prepared graphics build tools/switch/dksh_cache natively (issue #88)
        sb.AppendLine("apt-get install -y make gcc g++ zlib1g-dev git wget ca-certificates apt-transport-https");
        sb.AppendLine("if ! command -v dkp-pacman >/dev/null; then");
        sb.AppendLine("  mkdir -p /usr/share/keyring");
        sb.AppendLine($"  [ -s {key} ] || wget -nv -U 'dkp apt' -O {key} https://apt.devkitpro.org/devkitpro-pub.gpg");
        sb.AppendLine($"  [ -f {list} ] || echo 'deb [signed-by={key}] https://apt.devkitpro.org stable main' > {list}");
        sb.AppendLine("  apt-get update");
        sb.AppendLine("  apt-get install -y devkitpro-pacman");
        sb.AppendLine("fi");
        // pkg.devkitpro.org may answer 403 (#28): then again with the downloads through our Python, and last from
        // the package lists already there
        sb.Append(PacmanXferFunction("dkp-pacman"));
        sb.AppendLine($"dkp-pacman -Sy --needed --noconfirm {DkpPackages} || dkp_xfer || dkp-pacman -S --needed --noconfirm {DkpPackages}");
        return sb.ToString();
    }

    static string? PackageManager() =>
        new[] { "apt-get", "dnf", "pacman", "zypper" }.FirstOrDefault(pm => FindOnPath(pm) != null);

    static string PackageScript(string pm, Dictionary<string, string> byManager) =>
        byManager.TryGetValue(pm, out var cmd) ? "set -e\n" + cmd + "\n" : throw new InstallException(Str.Get("lx_tc_unknown"));

    // Runs a shell script as root: directly when the installer already is root, through pkexec when a desktop
    // session can ask for the password, else the user runs it with sudo in a terminal (the notice shows the command).
    void RunAsRoot(string what, string script)
    {
        var path = Path.Combine(WorkDir, "root-step.sh");
        File.WriteAllText(path, "#!/bin/sh\n# SwitchWakerHD installer: " + what + "\n" + script);
        OnLog(script.Trim(), LogKind.Output);

        var output = new List<string>();
        if (geteuid() == 0)
        {
            Say(Str.Get("lx_root_direct", what));
            var rc = Exec("/bin/sh", Quote(path), check: false, onLine: output.Add);
            if (rc != 0) Warn(Str.Get("lx_root_failed", rc) + " " + Str.Get(PacmanFailure(output)));
            return;
        }
        var graphical = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DISPLAY")) ||
                        !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("WAYLAND_DISPLAY"));
        if (graphical && FindOnPath("pkexec") != null)
        {
            Say(Str.Get("lx_root_pkexec", what));
            var rc = Exec("pkexec", "/bin/sh " + Quote(path), check: false, onLine: output.Add);
            if (rc == 0) return;
            // 126: the password window was dismissed; 127: no polkit agent, or not allowed
            Warn(Str.Get("lx_root_failed", rc) + (rc is 126 or 127 ? "" : " " + Str.Get(PacmanFailure(output))));
            if (rc is not (126 or 127)) return;   // it ran and failed: the sudo command would fail the same way
        }
        AskCommand?.Invoke(Str.Get("lx_root_h"), Str.Get("lx_root_body", what), Str.Get("btn_done_continue"), $"sudo sh {ShQuote(path)}");
    }

    // ── Build ────────────────────────────────────────────────────────────────
    protected override void BuildEnv(Dictionary<string, string> env)
    {
        env["PATH"] = Path.Combine(PyDir, "bin") + ":" + (Environment.GetEnvironmentVariable("PATH") ?? "/usr/bin:/bin");
        if (containerEngine != null) { env["WWHD_NATIVE"] = "0"; return; }
        env["DEVKITPRO"] = Dkp;
        env["WWHD_NATIVE"] = "1";
    }

    protected override double TotalRamGb()
    {
        try
        {
            var m = Regex.Match(File.ReadAllText("/proc/meminfo"), @"MemTotal:\s+(\d+) kB");
            return m.Success ? long.Parse(m.Groups[1].Value) / 1048576.0 : 0;
        }
        catch (Exception) { return 0; }
    }

    // ── Processes ────────────────────────────────────────────────────────────
    public override void KillCurrent()
    {
        base.KillCurrent();
        // a container runs under the engine's daemon (or conmon), not under our process
        if (containerEngine == null) return;
        var (_, ids) = Capture(containerEngine, $"ps -q --filter ancestor={DkpImage}");
        foreach (var id in ids.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            Capture(containerEngine, "kill " + id.Trim());
    }

    // ── SD card (from the done page) ─────────────────────────────────────────
    // Mounted removable or hot-plugged block devices (card readers, hekate's UMS), from lsblk.
    public override List<Volume> SdVolumes()
    {
        var found = new List<Volume>();
        try
        {
            var (code, json) = Capture("lsblk", "-J -b -o PATH,MOUNTPOINT,RM,HOTPLUG,LABEL,SIZE,FSTYPE");
            if (code != 0) return found;
            using var doc = JsonDocument.Parse(json);
            void Walk(JsonElement devs)
            {
                foreach (var d in devs.EnumerateArray())
                {
                    string? S(string k) => d.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
                    bool Flag(string k) => d.TryGetProperty(k, out var v) &&
                        (v.ValueKind == JsonValueKind.True || (v.ValueKind == JsonValueKind.String && v.GetString() == "1"));
                    long Num(string k) => d.TryGetProperty(k, out var v)
                        ? (v.ValueKind == JsonValueKind.Number ? v.GetInt64() : long.TryParse(v.GetString(), out var n) ? n : 0) : 0;
                    var mount = S("mountpoint");
                    if (!string.IsNullOrEmpty(mount) && mount != "/" && !mount.StartsWith("/boot") && mount != "[SWAP]" &&
                        (Flag("rm") || Flag("hotplug")))
                    {
                        var label = S("label");
                        found.Add(new Volume(mount!, string.IsNullOrEmpty(label) ? Path.GetFileName(mount!) : label!, Num("size"), S("fstype") ?? ""));
                    }
                    if (d.TryGetProperty("children", out var ch)) Walk(ch);
                }
            }
            if (doc.RootElement.TryGetProperty("blockdevices", out var root)) Walk(root);
        }
        catch (Exception) { }
        return found;
    }

    // Unmounts the card and powers its reader off, as the file manager's eject does (udisks); else gio.
    public override bool Eject(string mount)
    {
        var (code, src) = Capture("findmnt", "-no SOURCE " + Quote(mount));
        var dev = src.Trim();
        if (code == 0 && dev.StartsWith("/dev/") && Capture("udisksctl", "unmount -b " + Quote(dev)).code == 0)
        {
            Capture("udisksctl", "power-off -b " + Quote(Regex.Replace(dev, @"(?<=\D)\d+$|(?<=\dp)\d+$|p\d+$", "")));
            return true;
        }
        return Capture("gio", "mount -e " + Quote("file://" + mount)).code == 0;
    }

    public override void Open(string path, bool text = false)
    {
        try
        {
            var psi = new ProcessStartInfo("xdg-open") { UseShellExecute = false };
            psi.ArgumentList.Add(path);
            Process.Start(psi)?.Dispose();
        }
        catch (Exception) { }
    }
}
