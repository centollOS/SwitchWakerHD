#!/usr/bin/env python3
"""Train the profile-guided build on the console (docs/pgo.md).

The console runs a WWHD_PGO=GEN build (instrumented hot game files and runtime frame-thread code). For each
scene this visits it, waits for the place to load, zeroes the counters ("pgo reset"), lets the game run for a
dwell time and writes the counters ("pgo dump": libgcov adds them to the .gcda files in sdmc:/switch/wwhd/pgo/),
then fetches the files to build/pgo/ for `cmake -DWWHD_PGO=USE`.

  pgo_train.py train [--nro build/switch-pgo-gen/wwhd.nro] [--dwell 30] [--settle 20] [--plan default|heldout|all]
        deploys the training NRO (unless --no-deploy), boots to gameplay, visits the plan's scenes, fetches the files
  pgo_train.py fetch [--out build/pgo]     only fetches what is on the console
  pgo_train.py clean                       removes the .gcda files from the console (a new training starts from zero)

Scenes are the performance test's portable states (tools/switch/perf_states, played at two times of day) and the
Warp tab's places (the cheats' warps, which load places the states do not cover). A profile made on the scenes that
the performance test later judges would flatter the result: the plans differ in what they leave out.
  default   every state and warp except the held-out scenes below
  heldout   only the scenes that "default" leaves out (to check a profile on places it never saw)
  all       everything (the profile for a release build; judge it on a different console session or by play)
The held-out scenes: lava, windfall, dragonroost, outset_village (and warps 2, 3, 4).

Training runs at a fraction of the normal speed (counters in every hot block), so the dwell is in wall-clock
seconds and the game does fewer frames than usual; the counts are per frame, not per second, and keep their
proportions. Never judge speed on a training build.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_test as pt  # noqa: E402
import wwhd_debug as dbg  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "build" / "pgo"
REMOTE = "pgo"

HELD_OUT_STATES = ["lava", "windfall", "dragonroost", "outset_village"]
HELD_OUT_WARPS = [2, 3, 4]
# (kind, name, time of day) ; the day states at the performance test's time and a night variant
STATES = ["outset", "foresthaven", "forbidden", "sea_totg", "hyrule", "outset_grass", "outset_lookout", "outset_cliff",
          "lava", "windfall", "dragonroost", "outset_village"]
WARPS = [0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16]
DAY, NIGHT = 200.0, 20.0
# The Outset states are made for the afternoon with no story event due: at night the game halts on its own assertion
# (d_event_data.cpp:1103 "demo_data", 2026-10-23). These survived the night.
NIGHT_OK = {"outset", "foresthaven", "forbidden", "sea_totg", "hyrule"}


def plan(which):
    items = []
    for s in STATES:
        held = s in HELD_OUT_STATES
        for t in ((DAY, NIGHT) if s in NIGHT_OK else (DAY,)):
            items.append(("state", s, t, held))
    for w in WARPS:
        items.append(("warp", str(w), DAY, w in HELD_OUT_WARPS))
    if which == "default":
        return [i for i in items if not i[3]]
    if which == "heldout":
        return [i for i in items if i[3]]
    return items


def visit(con, item, settle, dwell):
    kind, name, tod, _ = item
    pt.log(f"{kind} {name} (time {tod:.0f}): load")
    con.cmd("bench", "clock", "freeze", f"{tod:.2f}")
    if kind == "state":
        arrivals = int(con.status().get("arrivals", "0"))
        con.cmd("bench", "load", name)
        end = time.time() + 120
        while time.time() < end and int(con.status().get("arrivals", "0")) <= arrivals:
            time.sleep(1.0)
        if int(con.status().get("arrivals", "0")) <= arrivals:
            pt.log(f"  {name}: the state did not arrive; skipped")
            return False
    else:
        con.cmd("warp", name)
    time.sleep(settle)  # the place's files, textures and shaders (the instrumented game is slow)
    con.cmd("pgo", "reset")
    time.sleep(dwell)
    pt.log("  " + con.cmd("pgo", "dump").strip())
    return True


def fetch(con, out):
    out.mkdir(parents=True, exist_ok=True)
    text = con.try_cmd("ls", REMOTE) or ""
    n = total = 0
    for line in text.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0] == "f" and parts[2].endswith(".gcda"):
            c = dbg.Conn(con.host, con.port, timeout=10.0)
            try:
                with open(out / parts[2], "wb") as f:
                    c.command("get", f"{REMOTE}/{parts[2]}", out=f, timeout=120.0)
            finally:
                c.close()
            n += 1
            total += (out / parts[2]).stat().st_size
    pt.log(f"fetched {n} .gcda files ({total / 1e6:.1f} MB) to {out}")
    return n


def cmd_train(a):
    con = pt.Console(a.host)
    pt.QUEST_LOG = a.quest_log
    if not a.no_deploy:
        con.deploy(a.nro)
    if con.try_cmd("pgo", "status") is None:
        raise SystemExit("this build has no 'pgo' command: it must be a WWHD_PGO=GEN build")
    items = plan(a.plan)[a.skip_first:]
    states = sorted({i[1] for i in items if i[0] == "state"})
    pt.ensure_states(con, states)
    pt.boot_to_gameplay(con, reload=False)
    con.try_cmd("pgo", "reset")
    done = 0
    for item in items:
        try:
            done += bool(visit(con, item, a.settle, a.dwell))
        except (RuntimeError, OSError) as e:
            pt.log(f"  {item[1]}: {e}")
    pt.log(f"{done} of {len(items)} scenes trained")
    fetch(con, Path(a.out))


def cmd_fetch(a):
    fetch(pt.Console(a.host), Path(a.out))


def cmd_clean(a):
    con = pt.Console(a.host)
    text = con.try_cmd("ls", REMOTE) or ""
    for line in text.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0] == "f" and parts[2].endswith(".gcda"):
            con.cmd("rm", f"{REMOTE}/{parts[2]}")
    pt.log("removed the old .gcda files")


def host_from_files():
    import os
    if os.environ.get("WWHD_SWITCH_HOST"):
        return os.environ["WWHD_SWITCH_HOST"]
    p = REPO / "build" / "switch_host.txt"
    if p.exists():
        return p.read_text().splitlines()[0].strip()
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("train", "fetch", "clean"):
        p = sub.add_parser(name)
        p.add_argument("--host", default=host_from_files())
        if name != "clean":
            p.add_argument("--out", default=str(OUT))
    t = sub.choices["train"]
    t.add_argument("--nro", default=str(REPO / "build" / "switch-pgo-gen" / "wwhd.nro"))
    t.add_argument("--no-deploy", action="store_true")
    t.add_argument("--plan", choices=["default", "heldout", "all"], default="default")
    t.add_argument("--dwell", type=float, default=30.0)
    t.add_argument("--settle", type=float, default=20.0)
    t.add_argument("--quest-log", type=int, default=1)
    t.add_argument("--skip-first", type=int, default=0, help="resume: leave out the first N scenes of the plan (done in an earlier run)")
    a = ap.parse_args()
    if not a.host:
        raise SystemExit("the console's address: build/switch_host.txt, WWHD_SWITCH_HOST or --host")
    {"train": cmd_train, "fetch": cmd_fetch, "clean": cmd_clean}[a.cmd](a)


if __name__ == "__main__":
    main()
