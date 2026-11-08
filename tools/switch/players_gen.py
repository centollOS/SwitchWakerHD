#!/usr/bin/env python3
"""SwitchWakerHD: the game code as the players' builds translate it (make_sd.py, step 3), for anything that must
match it: the PGO training build above all (#130: v1.2.1's EU profile was trained on code made by a plain recomp.py,
which keeps the frame-interpolation hooks the Switch build leaves out, so no player's EU build could use it).

  python3 -I tools/switch/players_gen.py RPX OUT
        runs tools/recomp/recomp.py RPX OUT with the players' options (WWHD_RECOMP_SWITCH_SKIP=1), then prints the hot
        files' fingerprint
  python3 -I tools/switch/players_gen.py --fingerprint DIR
        prints DIR's fingerprint as cmake/SwitchPgo.cmake computes it (a release profile's manifest.json has it as
        hot_sha256) and whether DIR is the players' code

Standard library only.
"""
import hashlib
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# what tools/recomp/fork_passes.py (fork_skip_forwarders) writes into table.c for each hook it leaves out
SKIP_MARK = b"hook left out (switch_skip_hooks.txt)"


def players_env(env=None):
    """recomp.py's environment for the players' code: the given one (default os.environ) with the Switch's hook skip
    list, unless it already sets WWHD_RECOMP_SWITCH_SKIP (a developer's own choice)"""
    env = dict(os.environ if env is None else env)
    # (upstream sync bfd7dd6) the Switch build leaves out the frame-interpolation hooks main did not have
    # (tools/recomp/switch_skip_hooks.txt: at 30 fps they only cost the main thread)
    env.setdefault("WWHD_RECOMP_SWITCH_SKIP", "1")
    return env


def hot_fingerprint(gen):
    """cmake/SwitchPgo.cmake's _switch_pgo_hot_sha256: one SHA-256 over each code_hot_*.c's name and SHA-256"""
    names = sorted(n for n in os.listdir(gen) if n.startswith("code_hot_") and n.endswith(".c"))
    text = ""
    for n in names:
        with open(os.path.join(gen, n), "rb") as f:
            text += "%s %s\n" % (n, hashlib.sha256(f.read()).hexdigest())
    return hashlib.sha256(text.encode()).hexdigest(), len(names)


def is_players_gen(gen):
    """whether gen/table.c has the forwarders of the hooks the Switch build leaves out"""
    try:
        with open(os.path.join(gen, "table.c"), "rb") as f:
            return SKIP_MARK in f.read()
    except OSError:
        return False


def report(gen):
    sha, n = hot_fingerprint(gen)
    print("%s: hot files' fingerprint %s (%d files); %s" % (
        gen, sha, n, "the players' code (WWHD_RECOMP_SWITCH_SKIP=1)" if is_players_gen(gen) else
        "NOT the players' code: made without WWHD_RECOMP_SWITCH_SKIP=1 (a PGO profile trained on it fits no player)"))


def main(argv):
    if len(argv) == 2 and argv[0] == "--fingerprint":
        report(argv[1])
        return 0
    if len(argv) != 2 or argv[0].startswith("-"):
        sys.exit(__doc__)
    rpx, out = argv
    env = players_env()
    if env["WWHD_RECOMP_SWITCH_SKIP"] != "1":
        print("players_gen: warning: WWHD_RECOMP_SWITCH_SKIP=%s from the environment: not the players' code"
              % env["WWHD_RECOMP_SWITCH_SKIP"], flush=True)
    rc = subprocess.call([sys.executable, "-I", os.path.join(ROOT, "tools", "recomp", "recomp.py"), rpx, out], env=env)
    if rc != 0:
        return rc
    report(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
