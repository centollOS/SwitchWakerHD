#!/usr/bin/env python3
"""SwitchWakerHD: guest mods (Mod SDK v2, docs/mod-sdk-v2.md) built into the Switch NRO.

The console cannot compile or load a mod's module, so the mods are chosen when the NRO is built: this script
runs each mod's setup on this computer, translates its PowerPC code to C (tools/guestmod/guestmod.py) and writes
what the build and the SD card need (docs/guest-mods-switch-plan.md):

  OUT/hooks.txt                 the game functions the mods hook (canonical addresses): the recompile's
                                WWHD_RECOMP_MOD_HOOKS_ONLY, so only those get the mods' check
  OUT/registry.c, OUT/mod_*.c   compiled into the NRO when OUT is build/guest-mods (cmake/SwitchPort.cmake)
  OUT/sd/<id>/                  sdmc:/switch/wwhd/mods/<id>/: package/ (manifest, images), options.json
  DATA/<id>/                    the mod's setup output (its data/ on the SD card): outside the checkout, since a
                                mod's files derived from your games must not land in a Git repository; DATA is
                                --data, by default a per-user folder (user_data_root)
  OUT/guest-mods.json           what was built (each mod's data folder too)

usage: guest_mods.py PACKAGE [PACKAGE ...] [--out build/guest-mods] [--data DIR] [--build USA|EU]
                     [--gc-game PATH] [--option ID.KEY=VALUE ...]
       guest_mods.py --clear [--out build/guest-mods]          (no mods: the NRO as without this script)

PACKAGE is a mod's .zip from the catalogue or its folder (manifest.json with kind "guest" and its mod.elf).
--gc-game is your GameCube Wind Waker (.iso, .gcm, .rvz or an extracted folder) for mods whose setup reads it.
After this, the game code must be translated again with the hook list (make_sd.py --guest-mod does both):
  WWHD_RECOMP_MOD_HOOKS_ONLY=build/guest-mods/hooks.txt python3 tools/recomp/recomp.py cking.rpx build/gen
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "guestmod"))
import build_guest_mod  # noqa: E402
import guestmod  # noqa: E402
from builds import by_name, canonical_build  # noqa: E402

DEFAULT_HEAP = 256 * 1024  # packages.cpp: guest.heap_size when the manifest gives none
# what the console's copy of a package keeps: the HUD loads package images only from these folders
# (guest_mods.cpp svc_hud_texture); the code is in the NRO
PACKAGE_DIRS = ("assets", "textures")
# the module's runtime entry points (guestmod.py's prelude): defined once per module, so renamed per mod
MODULE_SYMBOLS = ("ppc_dispatch", "ppc_unimplemented", "ppc_trap", "ppc_timebase", "ppc_fres", "ppc_frsqrte")
# a write of a paired-single quantization register: the game code assumes the GQRs it never writes keep their
# values (fork_passes.py, static GQRs), so a mod must not write any
GQR_WRITE = re.compile(r"c->gqr\[\d\]\s*=(?!=)")


class Fail(Exception):
    pass


def user_data_root():
    """The per-user folder for the mods' setup output (macOS Application Support, Windows LOCALAPPDATA, XDG)."""
    home = os.path.expanduser("~")
    if sys.platform == "darwin":
        base = os.path.join(home, "Library", "Application Support")
    elif os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.join(home, "AppData", "Local")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    return os.path.join(base, "SwitchWakerHD", "guest-mods")


def c_name(mod_id):
    return re.sub(r"[^a-z0-9_]", "_", mod_id)


def unpack(src, work):
    """A package folder: as it is; a .zip: extracted into work/<name> (paths inside it only, no links)."""
    if os.path.isdir(src):
        return os.path.abspath(src)
    if not zipfile.is_zipfile(src):
        raise Fail("%s: not a mod folder or .zip" % src)
    dst = os.path.join(work, os.path.splitext(os.path.basename(src))[0])
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    with zipfile.ZipFile(src) as z:
        for info in z.infolist():
            name = info.filename
            parts = name.replace("\\", "/").split("/")
            if name.startswith("/") or ":" in name or ".." in parts or (info.external_attr >> 28) == 0xA:
                raise Fail("%s: unsafe path in the archive: %s" % (src, name))
            if info.is_dir():
                continue
            out = os.path.join(dst, *parts)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(info) as i, open(out, "wb") as o:
                shutil.copyfileobj(i, o)
    return dst


def option_values(man, given):
    """The manifest's option defaults, with --option values checked against their declarations."""
    decl = {o["id"]: o for o in man.get("options", []) if isinstance(o, dict) and isinstance(o.get("id"), str)}
    values = {k: o["default"] for k, o in decl.items() if "default" in o}
    for key, text in given.items():
        o = decl.get(key)
        if o is None:
            raise Fail("%s: no option %r (it has: %s)" % (man["id"], key, ", ".join(sorted(decl)) or "none"))
        kind = o.get("type")
        if kind == "number":
            try:
                v = float(text)
            except ValueError:
                raise Fail("%s.%s: not a number: %s" % (man["id"], key, text))
            if ("min" in o and v < o["min"]) or ("max" in o and v > o["max"]):
                raise Fail("%s.%s: %s is outside %s-%s" % (man["id"], key, text, o.get("min"), o.get("max")))
            values[key] = int(v) if v == int(v) else v
        elif kind == "enum":
            if text not in o.get("choices", []):
                raise Fail("%s.%s: %s is not one of %s" % (man["id"], key, text, ", ".join(o.get("choices", []))))
            values[key] = text
        elif kind in ("bool", "boolean"):
            if text not in ("0", "1", "true", "false"):
                raise Fail("%s.%s: 0/1 or true/false" % (man["id"], key))
            values[key] = text in ("1", "true")
        else:
            values[key] = text
    return values


def run_setup(man, pkg, data, gc_game):
    """The manifest's setup steps that run on the computer: game_path, run_tool; build_guest_mod is this script."""
    games = {}
    for st in man.get("setup", []):
        kind = st.get("type")
        if kind == "game_path":
            if st.get("game") != "gc_wind_waker":
                raise Fail("%s: setup needs %s, which this script cannot give" % (man["id"], st.get("game")))
            if not gc_game:
                raise Fail("%s needs your GameCube Wind Waker (.iso, .gcm, .rvz or an extracted folder): --gc-game PATH"
                           % man["id"])
            games["gc_wind_waker"] = os.path.abspath(gc_game)
        elif kind == "run_tool":
            tool = os.path.normpath(os.path.join(pkg, st["tool"]))
            if not tool.startswith(pkg + os.sep) or not os.path.isfile(tool):
                raise Fail("%s: setup tool %s is not in the package" % (man["id"], st["tool"]))

            def arg(a):
                a = a.replace("{data}", data)
                return re.sub(r"\{game:(\w+)\}", lambda m: games[m.group(1)], a)
            print("  %s: %s" % (man["id"], st.get("title", st["tool"])), flush=True)
            # (not -I: the tool imports its own modules from its folder)
            rc = subprocess.call([sys.executable, "-s", "-E", tool] + [arg(a) for a in st.get("arguments", [])], cwd=pkg)
            if rc:
                raise Fail("%s: setup step %s failed (exit code %d)" % (man["id"], st.get("id"), rc))
            for o in st.get("outputs", []):
                if not os.path.isfile(os.path.join(data, o)):
                    raise Fail("%s: setup step %s made no %s" % (man["id"], st.get("id"), o))
        elif kind != "build_guest_mod":
            raise Fail("%s: setup step type %r is not supported for the Switch build" % (man["id"], kind))


def build(args):
    out = os.path.abspath(args.out)
    game_build = by_name(args.build)
    if game_build is None:
        raise Fail("unknown game build %s" % args.build)
    work = os.path.join(out, "work")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(work)
    given = {}
    for o in args.option:
        m = re.fullmatch(r"([a-z0-9_][a-z0-9_.-]*?)\.([A-Za-z0-9_-]+)=(.*)", o)
        if not m:
            raise Fail("--option %s: ID.KEY=VALUE" % o)
        given.setdefault(m.group(1), {})[m.group(2)] = m.group(3)
    base = guestmod.REGION_START
    mods, hooks = [], {}
    for src in args.package:
        pkg = unpack(src, work)
        man, elf = build_guest_mod.package_elf(pkg)
        mid = man["id"]
        if any(m["id"] == mid for m in mods):
            raise Fail("%s given twice" % mid)
        heap = man.get("guest", {}).get("heap_size", DEFAULT_HEAP)
        heap = (int(heap) + 15) & ~15
        t = build_guest_mod.translator_for(elf, base, game_build)
        canon = build_guest_mod.translator_for(elf, base, canonical_build())  # the hook targets as the lists name them
        region = (((t.end - base + 0xFFFF) & ~0xFFFF) + heap + 0xFFFF) & ~0xFFFF
        if base + region > guestmod.REGION_END:
            raise Fail("the mods do not fit the guest mod region")
        for kind, target, _, _ in canon.hooks:
            if kind == 1:
                # a replacement changes what callers of the function may assume (the fork's whole-program passes)
                raise Fail("%s replaces %08X: replacements are not supported in the Switch build (hooks are)" % (mid, target))
            hooks.setdefault(target, []).append(mid)
        source = t.emit(mid)
        if GQR_WRITE.search(source):
            raise Fail("%s writes a GQR (paired-single quantization register): not supported in the Switch build" % mid)
        sd = os.path.join(out, "sd", mid)
        os.makedirs(sd)
        data = os.path.join(os.path.abspath(args.data), mid)
        shutil.rmtree(data, ignore_errors=True)
        os.makedirs(data)
        run_setup(man, pkg, data, args.gc_game)
        os.makedirs(os.path.join(sd, "package"))
        shutil.copyfile(os.path.join(pkg, "manifest.json"), os.path.join(sd, "package", "manifest.json"))
        for d in PACKAGE_DIRS:
            if os.path.isdir(os.path.join(pkg, d)):
                shutil.copytree(os.path.join(pkg, d), os.path.join(sd, "package", d))
        with open(os.path.join(sd, "options.json"), "w") as f:
            json.dump(option_values(man, given.pop(mid, {})), f, indent=1)
        cn = c_name(mid)
        os.makedirs(os.path.join(out, "translated"), exist_ok=True)
        with open(os.path.join(out, "translated", cn + ".c"), "w") as f:
            f.write(source)
        with open(os.path.join(out, "mod_%s.c" % cn), "w") as f:
            f.write("/* Generated by tools/switch/guest_mods.py: guest mod %s %s, built into the NRO. Do not edit. */\n"
                    % (mid, man.get("version", "")))
            for s in MODULE_SYMBOLS:
                f.write("#define %s gm_%s_%s\n" % (s, cn, s))
            f.write("#define wwhd_guest_module_v1 wwhd_guest_builtin_%s\n" % cn)
            f.write('#include "translated/%s.c"\n' % cn)
        mods.append({"id": mid, "name": man.get("name", mid), "version": man.get("version", ""), "c_name": cn, "data": data,
                     "base": base, "region": region, "heap": heap, "memory": t.end - base,
                     "hooks": ["%08X" % tg for _, tg, _, _ in canon.hooks], "services": t.services})
        print("  %s %s: %d functions, %d hooks, guest memory %08X-%08X (module %d bytes, heap %d)" % (
            mid, man.get("version", ""), len(t.entries), len(t.hooks), base, base + region, t.end - base, heap), flush=True)
        base += region
    if given:
        raise Fail("--option for mods not given: %s" % ", ".join(sorted(given)))
    shutil.rmtree(work)
    with open(os.path.join(out, "hooks.txt"), "w") as f:
        f.write("# Generated by tools/switch/guest_mods.py: the game functions hooked by the NRO's guest mods\n")
        for tg in sorted(hooks):
            f.write("%08X  # %s\n" % (tg, ", ".join(hooks[tg])))
    with open(os.path.join(out, "registry.c"), "w") as f:
        f.write("/* Generated by tools/switch/guest_mods.py: the guest mods built into the NRO. Do not edit. */\n")
        f.write('#include "wwhd_guest_builtin.h"\n\n')
        for m in mods:
            f.write("const WWHDGuestModuleV1* wwhd_guest_builtin_%s(const WWHDGuestHostV1* host);\n" % m["c_name"])
        f.write("\nconst WWHDGuestBuiltin g_guest_builtin[] = {\n")
        for m in mods:
            f.write('    {"%s", "%s", "%s", wwhd_guest_builtin_%s, 0x%08Xu, 0x%Xu, 0x%Xu},\n' % (
                m["id"], m["name"].replace('"', "'"), m["version"], m["c_name"], m["base"], m["region"], m["heap"]))
        f.write("};\nconst unsigned g_guest_builtin_count = %d;\n" % len(mods))
    with open(os.path.join(out, "guest-mods.json"), "w") as f:
        json.dump({"build": game_build.name, "mods": mods}, f, indent=1)
    return mods


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package", nargs="*")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "guest-mods"))
    ap.add_argument("--data", default=user_data_root(), help="the mods' setup output, outside the checkout")
    ap.add_argument("--build", default="USA", help="the game's build (USA or EU)")
    ap.add_argument("--gc-game", help="your GameCube Wind Waker, for mods whose setup reads it")
    ap.add_argument("--option", action="append", default=[], help="a mod option: ID.KEY=VALUE")
    ap.add_argument("--clear", action="store_true", help="remove OUT: the NRO without guest mods")
    args = ap.parse_args()
    if args.clear:
        shutil.rmtree(os.path.abspath(args.out), ignore_errors=True)
        print("no guest mods: %s removed" % args.out)
        return
    if not args.package:
        ap.error("give one or more mod packages (or --clear)")
    try:
        mods = build(args)
    except (Fail, guestmod.ModError, OSError, KeyError, ValueError) as e:
        shutil.rmtree(os.path.abspath(args.out), ignore_errors=True)  # no half-made set reaches the build
        sys.exit("guest_mods: %s" % e)
    print("guest mods for the NRO: %s -> %s" % (", ".join(m["id"] for m in mods), args.out))


if __name__ == "__main__":
    main()
