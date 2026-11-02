// The installers' own Python (every system): python-build-standalone (Astral), a relocatable CPython unpacked into
// the work folder, and the packages of PythonLock.cs. Nothing comes from the player's computer: no Python on PATH,
// no py launcher, no pip configuration, no compiler (docs/pinned-toolchain-plan.md, step 2).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace SwitchWakerHD.Installer;

abstract partial class EngineBase
{
    // 3.14: compression.zstd (the minimap's Dolphin .rvz, a .wua) and wheels for every package in PythonLock.cs
    protected const string PyRelease = "20260929", PyVersion = "3.14.7";
    static readonly Dictionary<string, (string triple, string sha256)> PyBuilds = new()
    {
        ["win-x64"]     = ("x86_64-pc-windows-msvc",    "4990c28f72e15f11331dcb0cf4fb7e187b0bb134321ade04c265372bf9096571"),
        ["win-arm64"]   = ("aarch64-pc-windows-msvc",   "402e0c845a2bde240131285e146263651f4d25dc7def4d230f4341eed50a8391"),
        ["osx-arm64"]   = ("aarch64-apple-darwin",      "0211ccf04c5e0a0feb0b9bf1148030e1f6a31b8eae624f13919a7752a4e3dc79"),
        ["osx-x64"]     = ("x86_64-apple-darwin",       "18b12f5836c6a5cee8223f1759d26bd3174c1271bb3b7882dc09e2d961f3c863"),
        ["linux-arm64"] = ("aarch64-unknown-linux-gnu", "08a224bc0bcadfee3eab55db618343e6ba221f3595000e0894aae163648abc9d"),
        ["linux-x64"]   = ("x86_64-unknown-linux-gnu",  "0056208b5fdfcec939bc5b3908f5e7c89d11c6e2f1fdf02c9cc6aa98d10c5d05"),
    };

    static bool IsWindows => RuntimeInformation.IsOSPlatform(OSPlatform.Windows);
    protected string PyDir => Path.Combine(Work, "python");
    protected string PyExe => IsWindows ? Path.Combine(PyDir, "python.exe") : Path.Combine(PyDir, "bin", "python3");

    static string PyPlatform()
    {
        var os = IsWindows ? "win" : RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "osx" : "linux";
        var arch = RuntimeInformation.OSArchitecture switch
        {
            Architecture.X64 => "x64",
            Architecture.Arm64 => "arm64",
            var a => a.ToString().ToLowerInvariant(),
        };
        return os + "-" + arch;
    }

    // Unpacks a .tar.gz into a folder: the system's own tar, by its full path on Windows (System32's bsdtar; an
    // MSYS2 or Git tar on PATH reads C:\ paths differently).
    protected abstract void Untar(string archive, string intoDir);

    bool PinnedPythonWorks() =>
        File.Exists(PyExe) && Capture(PyExe, "-I -c \"import compression.zstd,ssl;print('PYOK')\"").output.Contains("PYOK");

    // The pinned Python (downloaded once, checked, unpacked into the work folder) with `packages` from
    // PythonLock.cs; sets pythonCmd.
    protected void PinnedPython(params string[] packages)
    {
        var platform = PyPlatform();
        if (!PyBuilds.TryGetValue(platform, out var build)) throw new InstallException(Str.Get("py_desk_arch", platform));
        if (!PinnedPythonWorks())
        {
            var name = $"cpython-{PyVersion}+{PyRelease}-{build.triple}-install_only.tar.gz";
            var tgz = Path.Combine(Work, name);
            if (!File.Exists(tgz))
            {
                Say(Str.Get("py_desk_downloading", PyVersion));
                var part = tgz + ".part";
                Download($"https://github.com/astral-sh/python-build-standalone/releases/download/{PyRelease}/{Uri.EscapeDataString(name)}", part);
                VerifySha256(part, build.sha256);
                File.Move(part, tgz);
            }
            Say(Str.Get("py_desk_unpacking"));
            if (Directory.Exists(PyDir)) Directory.Delete(PyDir, true);
            // the archive holds python/: unpacked next to it, as <work>/python
            Untar(tgz, Work);
            if (!PinnedPythonWorks()) throw new InstallException(Str.Get("py_desk_fail", PyDir));
        }
        pythonCmd = PyExe;
        Ok(Str.Get("py_found", $"Python {PyVersion} ({PyDir})"));

        Say(Str.Get("py_pkgs"));
        // exactly these files: each package at its pinned version with its wheels' hashes, wheels only (no compiler),
        // and pip's isolated mode (no PIP_* variables, no pip.ini / pip.conf, no user site)
        var req = Path.Combine(Work, "python-requirements.txt");
        File.WriteAllLines(req, packages.Select(p =>
        {
            var (version, sha256) = PythonLock.Packages[p];
            return $"{p}=={version} " + string.Join(" ", sha256.Select(h => "--hash=sha256:" + h));
        }));
        var args = "-I -m pip --isolated install --disable-pip-version-check --no-warn-script-location " +
                   $"--require-hashes --only-binary :all: --no-deps -r {Quote(req)}";
        if (Exec(pythonCmd, args, check: false) != 0)
            throw new InstallException(Str.Get("py_pkgs_fail", string.Join(" ", packages)));
    }
}
