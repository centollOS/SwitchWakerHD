#!/usr/bin/env python3
"""Release checklist F: the memory tour on the console (docs/release-checklist.md, docs/perf-results.md).

Warps through every third destination of the Warp tab (40 places; all of them hit the game's own assert at
M_DaiB, #18), stays 25 s in each, two laps by default, and at the end of each lap takes the game's log and its last
`[mem] malloc` line. One lap visits every place once, so what still grows in lap 2 is a leak.

    tools/switch/memory_tour.py --out perf/results/v1.0.1-memory.txt

The game must be running on the console with the debug server on (Dev tab); the tour starts a Quest Log as the
perf test does (perf_test.py boot_to_gameplay). Exit status 2: the console stopped answering (a crash?).
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import perf_test as pt  # noqa: E402


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=pt.dbg.default_host())
    p.add_argument("--out", required=True, help="the record: warps, stages and each lap's [mem] line")
    p.add_argument("--laps", type=int, default=2)
    p.add_argument("--dwell", type=float, default=25.0, help="seconds in each place (default 25)")
    p.add_argument("--warps", type=int, default=40, help="destinations per lap, every third (default 40)")
    a = p.parse_args()
    out = Path(a.out)
    logs = pt.OUT / f"memory_tour_{time.strftime('%Y%m%d_%H%M%S')}"
    logs.mkdir(parents=True, exist_ok=True)
    con = pt.Console(a.host)
    info = con.info()
    lines = [f"# checklist F, {info.get('version')}, {time.strftime('%Y-%m-%d')}: {a.warps} warps x {a.dwell:.0f} s, "
             f"{a.laps} laps"]
    pt.boot_to_gameplay(con, reload=False)
    warps = list(range(0, 136, 3))[:a.warps]
    t0 = time.time()

    def note(line):
        print(line, flush=True)
        lines.append(line)
        out.write_text("\n".join(lines) + "\n")

    for lap in range(1, a.laps + 1):
        for n in warps:
            con.try_cmd("warp", str(n), timeout=20.0)
            time.sleep(a.dwell)
            s = con.try_cmd("info", timeout=20.0)
            if s is None and con.wait_up(30):
                s = con.try_cmd("info", timeout=20.0)
            if s is None:
                note(f"lap {lap} warp {n}: NO ANSWER (crash?)")
                sys.exit(2)
            note(f"{time.time() - t0:7.0f}s lap {lap} warp {n:3d} stage {con.kv(s).get('stage', '?')}")
        note(f"--- end of lap {lap}: {time.time() - t0:.0f} s")
        log = logs / f"lap{lap}.log"
        subprocess.run([sys.executable, str(HERE / "wwhd_debug.py"), "--host", a.host, "lastlog", str(log)],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
        mem = [l for l in log.read_text(errors="replace").splitlines() if l.startswith("[mem] malloc")] \
            if log.exists() else []
        note("    " + (mem[-1] if mem else "no [mem] line"))
    note("tour done")
    print(f"logs: {logs}")


if __name__ == "__main__":
    main()
