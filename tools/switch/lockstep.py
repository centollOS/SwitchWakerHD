#!/usr/bin/env python3
"""Lockstep frame hashes on the console (docs/lockstep.md, issue #67).

Two builds, or one build with an option on and then off, play the same frames from the same portable state
(tools/switch/perf_states/NAME.wwstate) with the fixed seeds and the held clock, no input; the console hashes each
frame's picture (the one the window shows) (debug server "bench hash NAME N SKIP") and this script compares the lists and names the first
frame that differs.

  lockstep.py run [--nro NRO] [--label L] [--runs 2] [--scenes a,b] [--frames N] [--skip S]
        one build: every scene RUNS times; the runs must be identical (the determinism check). Lists in
        build/lockstep/<label>_<scene>_<run>.txt
  lockstep.py ab A.nro B.nro [--labels a,b] [--scenes ...]
        deploys A, hashes every scene, deploys B, hashes them again, compares; the console is left on B
  lockstep.py toggle --cmd-a "raw fsr 0" --cmd-b "raw fsr 1" [--check] [--scenes ...]
        one boot, a live switch: A, then B, per scene (--check: A once more, to show A is repeatable); the A
        command is sent again at the end
  lockstep.py compare A.txt B.txt      two saved lists

Every command except compare checks that the console answers, boots to a Quest Log (as perf_test.py does) unless
--no-boot, and leaves the clock running at the end. Nothing is saved: a portable load only changes the Quest Log in
memory. Nobody should touch the console during a run (its controller goes into the game).
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_test as pt  # noqa: E402
import wwhd_debug as dbg  # noqa: E402

OUT = pt.REPO / "build" / "lockstep"
MEM = False  # --mem: each run also snapshots the game's memory at its first hashed frame (a diagnostic)
DEFAULT_SCENES = ["lava", "windfall", "dragonroost"]
COLUMNS = ["step", "timer", "rnd0", "rnd1", "rnd2", "hash", "w", "h", "format", "draws", "held"]


def log(msg):
    pt.log(msg)


def parse(text):
    """{'meta': first comment line, 'frames': {step: row dict}}"""
    meta, frames = "", {}
    for line in text.splitlines():
        if line.startswith("# lockstep"):
            meta = line[2:]
        if not line or line.startswith(("#", "w ", "b ", "t ", "h ")):  # (w/b: the --mem diagnostic's lines)
            continue
        row = dict(zip(COLUMNS, line.split()))
        row["step"] = int(row["step"])
        frames[row["step"]] = row
    return {"meta": meta, "frames": frames}


def compare(a, b, name_a="A", name_b="B"):
    """(identical, the report lines)"""
    fa, fb = a["frames"], b["frames"]
    steps = sorted(set(fa) | set(fb))
    lines = []
    missing = [s for s in steps if s not in fa or s not in fb]
    if missing:
        lines.append(f"  {len(missing)} steps only in one list (the first: {missing[0]})")
    both = [s for s in steps if s in fa and s in fb]
    logic = [s for s in both if any(fa[s][k] != fb[s][k] for k in ("timer", "rnd0", "rnd1", "rnd2"))]
    picture = [s for s in both if fa[s]["hash"] != fb[s]["hash"]]
    size = [s for s in both if (fa[s]["w"], fa[s]["h"], fa[s]["format"]) != (fb[s]["w"], fb[s]["h"], fb[s]["format"])]
    draws = [s for s in both if fa[s]["draws"] != fb[s]["draws"]]
    held = [s for s in both if fa[s]["held"] == "1" or fb[s]["held"] == "1"]
    nohash = [s for s in both if int(fa[s]["hash"], 16) == 0 or int(fb[s]["hash"], 16) == 0]
    if not picture and not logic and not missing:
        lines.insert(0, f"  identical: {len(both)} frames (steps {both[0]}..{both[-1]})" if both else "  no frames")
    else:
        first = min(picture + logic + missing)
        lines.insert(0, f"  DIFFERENT from step {first}: pictures differ in {len(picture)} of {len(both)} frames, "
                        f"game state (mTimer, cM_rnd) in {len(logic)}")
        if logic:
            s = logic[0]
            lines.append(f"  game state first differs at step {s}: {name_a} timer {fa[s]['timer']} rnd {fa[s]['rnd0']} "
                         f"{fa[s]['rnd1']} {fa[s]['rnd2']}, {name_b} timer {fb[s]['timer']} rnd {fb[s]['rnd0']} "
                         f"{fb[s]['rnd1']} {fb[s]['rnd2']}")
            if picture and picture[0] < s:
                lines.append(f"  the picture differed earlier, at step {picture[0]}, with the same game state")
        elif picture:
            lines.append(f"  the game state is the same in every frame: a difference of the picture alone "
                         f"(the renderer, or what the game draws from the same state)")
        if picture:
            s = picture[0]
            lines.append(f"  first picture difference, step {s}: {name_a} {fa[s]['hash']} {fa[s]['w']}x{fa[s]['h']} "
                         f"draws {fa[s]['draws']}, {name_b} {fb[s]['hash']} {fb[s]['w']}x{fb[s]['h']} draws {fb[s]['draws']}")
    if size:
        lines.append(f"  picture size or format differs in {len(size)} frames (the first: step {size[0]})")
    if draws:
        lines.append(f"  draw counts differ in {len(draws)} frames (the first: step {draws[0]})")
    if held:
        lines.append(f"  {len(held)} frames were held for shader compiles (#49; the first: step {held[0]}): run again "
                     f"once the shaders are cached")
    if nohash:
        lines.append(f"  {len(nohash)} frames have no picture hash (no picture read back)")
    return not picture and not logic and not missing, lines


def differing(a, b):
    """(frames whose picture differs, frames compared)"""
    both = [s for s in a["frames"] if s in b["frames"]]
    return sum(1 for s in both if a["frames"][s]["hash"] != b["frames"][s]["hash"]), len(both)


def noise_verdict(a, b, a_again):
    """A against B weighed against A against A again: the scenes are not yet free of noise (docs/lockstep.md,
    "Determinism": a few frames in a hundred differ between two runs of the same build)"""
    ab, n = differing(a, b)
    aa, _ = differing(a, a_again)
    if not ab:
        return "verdict: no picture difference between A and B"
    if ab <= 2 * aa + 2:
        return f"verdict: A and B differ in {ab} of {n} frames, A twice in {aa}: within the runs' noise, not a difference"
    return f"verdict: A and B differ in {ab} of {n} frames, A twice in {aa}: a real difference"


def hash_scene(con, name, frames, skip, time_of_day):
    con.cmd("bench", "clock", "freeze", f"{time_of_day:.2f}")
    t0 = time.time()
    text = con.cmd("bench", "hash", name, str(frames), str(skip), *(["mem"] if MEM else []),
                   timeout=90 + (skip + frames) / 5 + 120)
    got = parse(text)
    log(f"{name}: {len(got['frames'])} frames hashed in {time.time() - t0:.0f} s ({got['meta']})")
    return text, got


def save(text, label, name, run=None):
    OUT.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    path = OUT / f"{label}_{name}{'' if run is None else f'_{run}'}_{stamp}.txt"
    path.write_text(text)
    return path


def prepare(con, a, scenes):
    if con.try_cmd("ping", timeout=5.0) is None:
        raise SystemExit(f"the console at {a.host} does not answer (start the game with the debug server on)")
    pt.QUEST_LOG = a.quest_log
    pt.ensure_states(con, scenes)


def report(title, ok, lines):
    print(f"{title}: {'IDENTICAL' if ok else 'DIFFERENT'}")
    for line in lines:
        print(line)


def cmd_run(a):
    con = pt.Console(a.host)
    scenes = scenes_arg(a)
    prepare(con, a, scenes)
    if a.nro:
        con.deploy(a.nro)
    log(f"console: {con.info().get('version')}")
    pt.boot_to_gameplay(con, reload=not a.no_boot and not a.nro)
    all_ok = True
    try:
        for name in scenes:
            runs = []
            for r in range(a.runs):
                text, got = hash_scene(con, name, a.frames, a.skip, a.time)
                log(f"  saved {save(text, a.label, name, r)}")
                runs.append(got)
            for r in range(1, len(runs)):
                ok, lines = compare(runs[0], runs[r], "run 1", f"run {r + 1}")
                all_ok &= ok
                report(f"{name}: run 1 against run {r + 1}", ok, lines)
    finally:
        con.try_cmd("bench", "clock", "run")
    return 0 if all_ok else 1


def cmd_ab(a):
    con = pt.Console(a.host)
    scenes = scenes_arg(a)
    labels = a.labels.split(",") if a.labels else ["A", "B"]
    prepare(con, a, scenes)
    got = {}
    try:
        for label, nro in zip(labels, [a.nro_a, a.nro_b]):
            con.deploy(nro)
            log(f"[{label}] console: {con.info().get('version')}")
            pt.boot_to_gameplay(con)
            for name in scenes:
                text, got[label, name] = hash_scene(con, name, a.frames, a.skip, a.time)
                log(f"  saved {save(text, label, name)}")
    finally:
        con.try_cmd("bench", "clock", "run")
    all_ok = True
    for name in scenes:
        ok, lines = compare(got[labels[0], name], got[labels[1], name], labels[0], labels[1])
        all_ok &= ok
        report(f"{name}: {labels[0]} against {labels[1]}", ok, lines)
    log(f"the console is left on {labels[1]} ({a.nro_b})")
    return 0 if all_ok else 1


def cmd_toggle(a):
    con = pt.Console(a.host)
    scenes = scenes_arg(a)
    prepare(con, a, scenes)
    if a.nro:
        con.deploy(a.nro)
    log(f"console: {con.info().get('version')}")
    pt.boot_to_gameplay(con, reload=not a.no_boot and not a.nro)
    cmds = {"A": a.cmd_a.split(), "B": a.cmd_b.split()}
    order = ["A", "B"] + (["A"] if a.check else [])
    all_ok = True
    try:
        for name in scenes:
            got = []
            for i, letter in enumerate(order):
                log(f"{name}: {' '.join(cmds[letter])} -> {con.cmd(*cmds[letter]).strip()}")
                text, g = hash_scene(con, name, a.frames, a.skip, a.time)
                log(f"  saved {save(text, f'toggle{letter}', name, i)}")
                got.append(g)
            ok, lines = compare(got[0], got[1], "A", "B")
            report(f"{name}: A ({a.cmd_a}) against B ({a.cmd_b})", ok, lines)
            all_ok &= ok
            if a.check:
                ok2, lines2 = compare(got[0], got[2], "A", "A again")
                report(f"{name}: A against A again (repeatable)", ok2, lines2)
                print(f"  {noise_verdict(got[0], got[1], got[2])}")
    finally:
        con.try_cmd(*cmds["A"])
        con.try_cmd("bench", "clock", "run")
    return 0 if all_ok else 1


def cmd_compare(a):
    ok, lines = compare(parse(Path(a.a).read_text()), parse(Path(a.b).read_text()), Path(a.a).name, Path(a.b).name)
    report(f"{a.a} against {a.b}", ok, lines)
    return 0 if ok else 1


def scenes_arg(a):
    return a.scenes.split(",") if a.scenes else DEFAULT_SCENES


def common(p):
    p.add_argument("--host", default=dbg.default_host())
    p.add_argument("--scenes", help=f"comma-separated state names of tools/switch/perf_states (default: {','.join(DEFAULT_SCENES)})")
    p.add_argument("--frames", type=int, default=300, help="frames hashed per scene (default 300: 10 s of game)")
    p.add_argument("--skip", type=int, default=90, help="logic steps after the arrival before the first hashed one "
                   "(default 90: Link has landed and the camera settled)")
    p.add_argument("--time", type=float, default=pt.FROZEN_TIME, help="time of day held (degrees, default 200)")
    p.add_argument("--no-boot", action="store_true", help="the game is already in a Quest Log: do not reload")
    p.add_argument("--mem", action="store_true",
                   help="diagnostic: also list the game memory that differs from the run before at the first hashed "
                        "frame (in the saved lists, lines 'w ADDR OLD NEW' and 'b START END')")
    p.add_argument("--quest-log", type=int, default=1, choices=(1, 2, 3), help="the Quest Log the file select starts")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run", help="one build, each scene several times (determinism)")
    common(p)
    p.add_argument("--nro", help="deploy this NRO first (else the game is reloaded unless --no-boot)")
    p.add_argument("--label", default="run")
    p.add_argument("--runs", type=int, default=2)
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("ab", help="two builds")
    common(p)
    p.add_argument("nro_a")
    p.add_argument("nro_b")
    p.add_argument("--labels", help="two labels, comma-separated (default A,B)")
    p.set_defaults(fn=cmd_ab)
    p = sub.add_parser("toggle", help="one build, a live debug-server switch")
    common(p)
    p.add_argument("--nro")
    p.add_argument("--cmd-a", required=True)
    p.add_argument("--cmd-b", required=True)
    p.add_argument("--check", action="store_true", help="A once more after B: A must be repeatable")
    p.set_defaults(fn=cmd_toggle)
    p = sub.add_parser("compare", help="two saved lists")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(fn=cmd_compare)
    a = ap.parse_args()
    global MEM
    MEM = getattr(a, "mem", False)
    if getattr(a, "host", "x") is None:
        raise SystemExit("the console's address: --host IP, WWHD_SWITCH_HOST=IP, or build/switch_host.txt")
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()
