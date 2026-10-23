#!/usr/bin/env python3
"""Reproducible performance test of SwitchWakerHD on the console (docs/perf-test.md).

Each scene is a portable state (tools/switch/perf_states/NAME.wwstate, copied to sdmc:/switch/wwhd/bench/) made
with Quest Log 2's progress, Link standing still with the player in control. For each scene the runner holds the
game clock at a fixed time of day, loads the state, waits a fixed settle time, then records N presented frames one by
one on the console (debug server "bench rec": frame time, render thread busy, main thread CPU, sampled GPU time, GPU
behind, draws, internal resolution, player control) and summarises them.

  perf_test.py run [--nro NRO] [--label L] [--runs R] [--scenes a,b] [--frames N] [--settle S]
        one build: optional deploy, boot to gameplay, every scene R times (scene order rotates per run);
        results in build/perf/<label>_<time>.json and .csv
  perf_test.py ab A.nro B.nro [C.nro ...] [--order ABBA] [--repeat 2] [--labels a,b,c] [...]
        interleaved comparison: each letter of the order (repeated) deploys that NRO and runs every scene once
        (default order A..Z then back, e.g. ABCCBA); prints each build against the first; leaves the console on
        --final (default: A)
  perf_test.py toggle --cmd-a "psmtx 0" --cmd-b "psmtx 1" [--order ABBA --repeat 3] [--scenes lava] [--frames N]
        A/B inside one boot: the scene is loaded once, the two commands alternate between recordings
  perf_test.py compare-to main|last-tag [--nro MINE.nro] [--rpx cking.rpx]
        the contributor's build against main or the last published tag (perf_baseline.py builds the baseline in
        build/perf/); the fast defaults (lava, windfall, dragonroost; 900 frames; ABBA twice), a verdict per scene,
        and the console left on the contributor's build. macOS and Linux (container build) only
  perf_test.py gpu-profile [--profiles 307,384] [--order ABBA] [--repeat 2] [--scenes outset_tallgrass]
        one build, the handheld GPU profile alternated between boots (settings.ini's switchGpuProfile, put back
        afterwards); each boot's clocks are checked against the profile (sys-clk must be off)
  perf_test.py compare R1.json R2.json ...   the comparison table of saved results
  perf_test.py show R.json             a saved result's table

Builds without the "bench" debug command cannot be measured (platform/bench_switch.cpp; v0.4.7 gets it from
perf_baselines/v0.4.7.patch).
The console's settings.ini is never changed (except by gpu-profile, which puts the player's file back at the end, also
after Ctrl+C or an error); the player's saves are never written (a portable load only changes
the Quest Log in memory; nothing here saves the game).
"""
import argparse
import json
import platform
import re
import subprocess
import statistics as st
import socket
import sys
import threading
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wwhd_debug as dbg  # noqa: E402
import perf_baseline  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
STATES = REPO / "tools" / "switch" / "perf_states"
OUT = REPO / "build" / "perf"
REMOTE_DIR = "bench"
# the default scenes: (state, what it stresses). Order is rotated between runs.
SCENES = [
    ("outset", "Outset from the lookout: far island, trees, sea (GPU and draws)"),
    ("windfall", "Windfall pier facing the town: NPCs, grass (render thread)"),
    ("dragonroost", "Dragon Roost Island beach (draws, GPU)"),
    ("lava", "Dragon Roost Cavern lava room: ~4900 draws (main thread)"),
    ("foresthaven", "Forest Haven interior: foliage, ~3800 draws (main + render)"),
    ("forbidden", "Forbidden Woods entrance (dungeon)"),
    ("sea_totg", "open sea on the boat at the Tower of the Gods (waves, light load)"),
]
EXTRA_SCENES = ["hyrule"]
# the demanding set (--scenes demanding; 'all' includes it): a player's Outset spots where the frame rate drops most,
# saved from a late-game file (docs/perf-test.md, "The scenes")
DEMANDING_SCENES = [
    ("outset_grass", "Outset, Link in the tall grass: ~6900 draws (render thread and GPU)"),
    ("outset_village", "Outset by Sue-Belle's house, ship and lookout in view: ~6400 draws (render, GPU and main thread)"),
    ("outset_lookout", "Outset, in the grass facing the lookout: ~3000 draws (main thread)"),
    ("outset_cliff", "Outset, the cliff above the sea: ~2250 draws (GPU)"),
]
# gpu-profile's default scene: Outset's tall grass seen from the sea stage (room 44), time of day 150, ~5200 draws;
# GPU-bound at the stock 307 MHz (European game)
GPU_SCENE, GPU_SCENE_TIME = "outset_tallgrass", 150.0
# the menu's handheld GPU profiles (platform/settings_switch.cpp kIds): switchGpuProfile value -> (GPU, memory) MHz.
# "307" is stock (builds before the 384 MHz default have no "307" id and read it as their default, stock); "default"
# is left out on purpose: it means the build's default, which changed (stock until 2026-10-23, then 384)
GPU_PROFILES = {"307": (307, 1331), "384": (384, 1331), "460": (460, 1331), "1600": (460, 1600)}
FROZEN_TIME = 200.0  # time of day held during every scene (degrees: afternoon)


class Console:
    def __init__(self, host, port=dbg.DEFAULT_PORT):
        self.host, self.port = host, port

    def cmd(self, *args, timeout=30.0):
        c = dbg.Conn(self.host, self.port, timeout=10.0)
        try:
            return c.command(*args, timeout=timeout).decode(errors="replace")
        finally:
            c.close()

    def try_cmd(self, *args, timeout=30.0):
        try:
            return self.cmd(*args, timeout=timeout)
        except (OSError, dbg.ServerError, ValueError):
            return None

    def kv(self, text):
        out = {}
        for line in (text or "").splitlines():
            k, _, v = line.partition(" ")
            out[k] = v
        return out

    def status(self):
        return self.kv(self.try_cmd("bench", "status"))

    def info(self):
        return self.kv(self.try_cmd("info"))

    def wait_up(self, seconds=120):
        end = time.time() + seconds
        while time.time() < end:
            if self.try_cmd("ping", timeout=5.0) is not None:
                return True
            time.sleep(1.0)
        return False

    def put(self, local, remote):
        c = dbg.Conn(self.host, self.port, timeout=10.0)
        try:
            data = Path(local).read_bytes()
            reply = c.command("put", remote, len(data), payload=data, timeout=max(60.0, len(data) / 1e5)).decode()
            want = f"crc32 {zlib.crc32(data) & 0xFFFFFFFF:08x} size {len(data)}"
            if reply != want:
                raise SystemExit(f"upload of {local} failed: {reply} (expected {want})")
        finally:
            c.close()

    def deploy(self, nro):
        log(f"deploying {nro}")
        self.put(nro, dbg.REMOTE_NRO)
        self.try_cmd("reload", timeout=10.0)
        time.sleep(4.0)
        if not self.wait_up(150):
            raise SystemExit("the game did not come back after the deploy (lastlog / crashes)")


class LogCounter:
    """counts log lines containing `needle` while running (the debug server's log stream)"""

    def __init__(self, con, needle):
        self.count, self.needle, self.stop = 0, needle.encode(), threading.Event()
        self.ok = False
        try:
            self.c = dbg.Conn(con.host, con.port, timeout=10.0)
            self.c.sock.sendall(b"log\n")
            self.ok, _ = self.c.header()
        except (OSError, ValueError):
            return
        self.c.sock.settimeout(0.5)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        pending = self.c.buf
        while not self.stop.is_set():
            lines = pending.split(b"\n")
            pending = lines.pop()
            self.count += sum(1 for line in lines if self.needle in line)
            try:
                data = self.c.sock.recv(1 << 16)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            pending += data

    def finish(self):
        if not self.ok:
            return None
        time.sleep(1.0)  # the log is flushed in batches
        self.stop.set()
        self.thread.join(2.0)
        self.c.close()
        return self.count


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def ensure_states(con, names):
    remote = con.try_cmd("ls", REMOTE_DIR) or ""
    have = {}
    for line in remote.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "f":
            have[parts[2]] = int(parts[1])
    if not have:
        con.try_cmd("mkdir", REMOTE_DIR)
    for n in names:
        local = STATES / f"{n}.wwstate"
        if not local.exists():
            raise SystemExit(f"no {local}")
        if have.get(local.name) != local.stat().st_size:
            log(f"copying {local.name} to the SD card ({REMOTE_DIR}/)")
            con.put(local, f"{REMOTE_DIR}/{local.name}")


QUEST_LOG = 1  # --quest-log: the Quest Log the file select starts


def boot_to_gameplay(con, reload=False):
    """from wherever the game is to a Quest Log being played with the player in control (QUEST_LOG is started:
    Down on the file select's list, once, before its first A; the benchmark states then replace its progress in
    memory only)"""
    if reload:
        log("reload")
        con.try_cmd("reload", timeout=10.0)
        time.sleep(4.0)
        if not con.wait_up(150):
            raise SystemExit("the game did not come back after reload")
    if con.try_cmd("bench", "status") is None:
        raise SystemExit("this build has no 'bench' debug command (docs/perf-test.md: main, or v0.4.8 and later)")
    end = time.time() + 240
    presses = 0
    picked = QUEST_LOG <= 1
    while time.time() < end:
        s = con.status()
        stage = s.get("stage", "-")
        if stage not in ("-", "", "sea_T", "Name") and s.get("control", "0").startswith("1"):
            log(f"in game: {stage}")
            return
        if stage in ("sea_T", "Name"):  # the title screen; "Name" is the file select (and the name entry)
            if presses > 12:
                raise SystemExit("could not get past the title screen / file select")
            if stage == "Name" and not picked:  # the list (the first Name after the title): only here, never on
                for _ in range(QUEST_LOG - 1):    # the Quest Log's own screen (Delete, Copy)
                    con.try_cmd("press", "DOWN")
                    time.sleep(0.8)
                picked = True
                log(f"Quest Log {QUEST_LOG} chosen")
            con.try_cmd("press", "A")
            presses += 1
            time.sleep(3.0)
            continue
        time.sleep(2.0)
    raise SystemExit("no gameplay within 4 minutes")


def pct(values, p):
    if not values:
        return None
    v = sorted(values)
    k = (len(v) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(v) - 1)
    return v[f] + (v[c] - v[f]) * (k - f)


def summarise(rows, no_control):
    dt = [r[0] for r in rows]
    total = sum(dt)
    gpu = [r[3] for r in rows if r[3] >= 0]
    # one-second windows of frames: the fps a player sees
    win, acc, n = [], 0, 0
    for d in dt:
        acc += d
        n += 1
        if acc >= 1e6:
            win.append(n * 1e6 / acc)
            acc, n = 0, 0
    return {
        "frames": len(rows),
        "fps": len(rows) * 1e6 / total,
        "fps_p5_window": pct(win, 5),
        "dt_p50_ms": pct(dt, 50) / 1000,
        "dt_p95_ms": pct(dt, 95) / 1000,
        "dt_p99_ms": pct(dt, 99) / 1000,
        "slow_pct": 100.0 * sum(1 for d in dt if d > 34500) / len(dt),
        "main_ms": st.mean(r[2] for r in rows) / 1000,
        "main_p50_ms": pct([r[2] for r in rows], 50) / 1000,
        "main_ms_per_pass": sum(r[2] for r in rows) / max(1, sum(r[4] for r in rows)) / 1000,
        "render_ms": st.mean(r[1] for r in rows) / 1000,
        "gpu_ms": st.median(gpu) / 1000 if gpu else None,
        "gpu_samples": len(gpu),
        "behind_pct": 100.0 * sum(r[6] for r in rows) / len(rows),
        "draws": st.median(r[5] for r in rows),
        "res": st.mean(r[8] for r in rows) / 100,
        "no_control": no_control + sum(1 for r in rows if not r[7]),
    }


def measure_scene(con, name, settle, frames, time_of_day):
    s0 = con.status()
    arrivals = int(s0.get("arrivals", "0"))
    con.cmd("bench", "clock", "freeze", f"{time_of_day:.2f}")
    con.cmd("bench", "load", name)
    end = time.time() + 90
    while time.time() < end:
        time.sleep(1.0)
        if int(con.status().get("arrivals", "0")) > arrivals:
            break
    else:
        raise RuntimeError(f"{name}: the state did not arrive within 90 s")
    time.sleep(settle)
    s = con.status()
    if not s.get("control", "0").startswith("1"):
        raise RuntimeError(f"{name}: no control after settling ({s.get('control')})")
    kills = LogCounter(con, "kill DSP channel")  # issue #13: the game's audio DSP-overload guard
    text = con.cmd("bench", "rec", str(frames), timeout=frames / 10 + 60)
    dsp_kills = kills.finish()
    rows, no_control = [], 0
    for line in text.splitlines():
        if line.startswith("# no_control_checks"):
            no_control = int(line.split()[-1])
        elif line and not line.startswith("#"):
            rows.append([int(x) for x in line.split()])
    res = summarise(rows, no_control)
    res["stage"] = s.get("stage")
    res["dsp_kills"] = dsp_kills
    return res, rows


def warm_up(con, scenes, seconds, time_of_day):
    """each scene loaded once, unmeasured: after a boot the first visit of a place reads its files and shaders"""
    if seconds <= 0:
        return
    log(f"warm-up: each scene for {seconds:.0f} s")
    con.cmd("bench", "clock", "freeze", f"{time_of_day:.2f}")
    for name in scenes:
        arrivals = int(con.status().get("arrivals", "0"))
        con.cmd("bench", "load", name)
        end = time.time() + 90
        while time.time() < end and int(con.status().get("arrivals", "0")) <= arrivals:
            time.sleep(1.0)
        time.sleep(seconds)


def run_set(con, label, scenes, runs, settle, frames, time_of_day, results, raw, rotate_from=0, warmup=0):
    warm_up(con, scenes, warmup, time_of_day)
    for r in range(runs):
        k = (rotate_from + r) % len(scenes)
        order = scenes[k:] + scenes[:k]
        for name in order:
            try:
                res, rows = measure_scene(con, name, settle, frames, time_of_day)
            except RuntimeError as e:
                log(f"[{label}] {e}")
                results.append({"build": label, "scene": name, "error": str(e)})
                continue
            res.update({"build": label, "scene": name, "run": len([x for x in results if x.get("build") == label
                                                                     and x.get("scene") == name])})
            results.append(res)
            raw.append({"build": label, "scene": name, "rows": rows})
            log(f"[{label}] {name:12s} {res['fps']:5.2f} fps  frame p95 {res['dt_p95_ms']:5.1f} ms  main "
                f"{res['main_ms']:5.1f} ms  render {res['render_ms']:5.1f} ms  gpu {res['gpu_ms'] or 0:5.1f} ms  "
                f"behind {res['behind_pct']:4.1f}%  draws {res['draws']:.0f}  res {res['res']:.2f}  dsp kills {res['dsp_kills']}"
                + (f"  NO CONTROL {res['no_control']}" if res["no_control"] else ""))


# two-sided 95% Student t, degrees of freedom 1..30 (beyond: 1.96)
T95 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131,
       2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]


def ci95(a, b):
    """half-width of the 95% interval of mean(b) - mean(a) (Welch: unequal spreads, few runs); 0 below 2 runs"""
    if len(a) < 2 or len(b) < 2:
        return 0.0
    va, vb = st.variance(a) / len(a), st.variance(b) / len(b)
    if va + vb == 0:
        return 0.0
    df = (va + vb) ** 2 / ((va ** 2 / (len(a) - 1) if va else 0) + (vb ** 2 / (len(b) - 1) if vb else 0))
    t = T95[max(1, int(df + 1e-9)) - 1] if df < 31 else 1.96  # (floored: the conservative side)
    return t * (va + vb) ** 0.5


def table(results, builds=None):
    ok = [r for r in results if "error" not in r]
    builds = builds or sorted({r["build"] for r in ok}, key=lambda b: [r["build"] for r in ok].index(b))
    scenes = []
    for r in ok:
        if r["scene"] not in scenes:
            scenes.append(r["scene"])

    def stats(b, sc):
        v = [r for r in ok if r["build"] == b and r["scene"] == sc]
        k = [r["dsp_kills"] for r in v if r.get("dsp_kills") is not None]
        return [r["fps"] for r in v], [r["main_ms"] for r in v], k

    sd = lambda x: st.stdev(x) if len(x) > 1 else 0.0
    lines = [f"{'scene':12s} {'build':16s} {'fps':>6s} {'sd':>5s} {'n':>2s} {'main ms':>8s} {'sd':>5s} {'kills':>6s}"
             + ("   vs " + builds[0] + ": fps (95% CI)        main ms (95% CI)" if len(builds) > 1 else "")]
    for sc in scenes:
        fa, ma, _ = stats(builds[0], sc)
        for b in builds:
            fps, main, k = stats(b, sc)
            if not fps:
                continue
            line = (f"{sc:12s} {b[:16]:16s} {st.mean(fps):6.2f} {sd(fps):5.2f} {len(fps):2d} {st.mean(main):8.2f} "
                    f"{sd(main):5.2f} {st.mean(k) if k else float('nan'):6.1f}")
            if b != builds[0] and fa:
                d, dm = st.mean(fps) - st.mean(fa), st.mean(main) - st.mean(ma)
                line += (f"   {d:+6.2f} (+-{ci95(fa, fps):4.2f}) {100 * d / st.mean(fa):+5.1f}%   "
                         f"{dm:+6.2f} (+-{ci95(ma, main):4.2f}) {100 * dm / st.mean(ma):+5.1f}%")
            lines.append(line)
    return "\n".join(lines)


def verdict(results, base, new):
    """one line per scene: how `new` compares with `base`, read against the 95% interval of the run spread (which
    holds the boot-to-boot spread). At the 30 fps cap the fps cannot move: the main thread's time is read instead."""
    ok = [r for r in results if "error" not in r]
    scenes = []
    for r in results:
        if r["scene"] not in scenes:
            scenes.append(r["scene"])
    lines = []
    for sc in scenes:
        fa = [r["fps"] for r in ok if r["build"] == base and r["scene"] == sc]
        fb = [r["fps"] for r in ok if r["build"] == new and r["scene"] == sc]
        ma = [r["main_ms"] for r in ok if r["build"] == base and r["scene"] == sc]
        mb = [r["main_ms"] for r in ok if r["build"] == new and r["scene"] == sc]
        if len(fa) < 2 or len(fb) < 2:
            lines.append(f"{sc}: not enough runs to compare ({base} {len(fa)}, {new} {len(fb)}; at least 2 each)")
            continue
        if st.mean(fa) >= 29.9 and st.mean(fb) >= 29.9:
            d = st.mean(mb) - st.mean(ma)
            ci = ci95(ma, mb)
            if abs(d) <= ci:
                lines.append(f"{sc}: no measurable change (at the 30 fps cap; main thread {d:+.2f} ms +-{ci:.2f})")
            else:
                lines.append(f"{sc}: at the 30 fps cap; main thread {d:+.2f} ms a frame (+-{ci:.2f}): "
                             + ("more headroom" if d < 0 else "less headroom"))
            continue
        d = st.mean(fb) - st.mean(fa)
        ci = ci95(fa, fb)
        pct_d = 100 * d / st.mean(fa)
        if abs(d) <= ci:
            lines.append(f"{sc}: no measurable change ({d:+.2f} fps +-{ci:.2f})")
        else:
            lines.append(f"{sc}: {'faster' if d > 0 else 'slower'}: {d:+.2f} fps (+-{ci:.2f}, {pct_d:+.1f}%)")
    return lines


def save(results, raw, label, meta):
    OUT.mkdir(parents=True, exist_ok=True)
    stem = OUT / f"{label}_{time.strftime('%Y%m%d_%H%M%S')}"
    Path(f"{stem}.json").write_text(json.dumps({"meta": meta, "results": results}, indent=1))
    keys = [k for k in results[0].keys()] if results else []
    for r in results:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(f"{stem}.csv", "w") as f:
        f.write(",".join(keys) + "\n")
        for r in results:
            f.write(",".join("" if r.get(k) is None else str(r.get(k)) for k in keys) + "\n")
    with open(f"{stem}_frames.json", "w") as f:
        json.dump(raw, f)
    log(f"saved {stem}.json / .csv / _frames.json")
    return stem


def scenes_arg(a):
    if not a.scenes:
        return [n for n, _ in SCENES]
    if a.scenes == "all":
        return [n for n, _ in SCENES] + EXTRA_SCENES + [n for n, _ in DEMANDING_SCENES]
    if a.scenes == "demanding":
        return [n for n, _ in DEMANDING_SCENES]
    return a.scenes.split(",")


def common(p):
    p.add_argument("--host", default=dbg.default_host())
    p.add_argument("--scenes", help="comma-separated state names (default: the 7 standard ones; 'demanding': the four Outset "
                   "stress spots; 'all': both and the extras)")
    p.add_argument("--frames", type=int, default=1800, help="frames recorded per scene (default 1800, 60 s at 30 fps)")
    p.add_argument("--settle", type=float, default=20.0, help="seconds after arriving before recording (default 20)")
    p.add_argument("--time", type=float, default=FROZEN_TIME, help="time of day held (degrees, default 200)")
    p.add_argument("--warmup", type=float, default=8.0,
                   help="after booting, visit each scene unmeasured for this many seconds (default 8; 0: none)")
    p.add_argument("--no-boot", action="store_true", help="the game is already in a Quest Log: do not reload")
    p.add_argument("--quest-log", type=int, default=1, choices=(1, 2, 3),
                   help="the Quest Log the file select starts (default 1)")


def cmd_run(a):
    con = Console(a.host)
    scenes = scenes_arg(a)
    if a.nro:
        con.deploy(a.nro)
    info = con.info()
    log(f"console: {info.get('version')}")
    ensure_states(con, scenes)
    boot_to_gameplay(con, reload=not a.no_boot and not a.nro)
    results, raw = [], []
    label = a.label or info.get("version", "build").split()[0]
    run_set(con, label, scenes, a.runs, a.settle, a.frames, a.time, results, raw, warmup=a.warmup)
    con.try_cmd("bench", "clock", "run")
    print(table(results))
    save(results, raw, label, {"version": info.get("version"), "nro": a.nro, "args": {k: v for k, v in vars(a).items() if k != "fn"}})


def run_ab(con, a, nro_list, names):
    """interleaved comparison of 2+ NROs: the order's letters are A, B, C... = the NROs in the order given"""
    scenes = scenes_arg(a)
    letters = "ABCDEFGH"[:len(nro_list)]
    nros = dict(zip(letters, [str(n) for n in nro_list]))
    if len(names) != len(nro_list):
        raise SystemExit("--labels needs one name per NRO")
    labels = dict(zip(letters, names))
    order = (a.order or (letters + letters[::-1])) * a.repeat
    if set(order) - set(letters):
        raise SystemExit(f"--order uses letters {letters} only")
    stem_name = "ab_" + "_vs_".join(names)
    results, raw = [], []
    ensure_states(con, scenes)
    for i, letter in enumerate(order):
        con.deploy(nros[letter])
        info = con.info()
        log(f"[{i + 1}/{len(order)}] {letter} {labels[letter]}: {info.get('version')}")
        boot_to_gameplay(con)
        run_set(con, labels[letter], scenes, 1, a.settle, a.frames, a.time, results, raw, rotate_from=i,
                warmup=a.warmup)
        save(results, raw, stem_name + "_partial", {"order": order, "nros": nros})
    print(table(results, names))
    save(results, raw, stem_name, {"order": order, "nros": nros,
                                   "args": {k: v for k, v in vars(a).items() if k != "fn"}})
    return results, nros


def cmd_ab(a):
    con = Console(a.host)
    names = a.labels.split(",") if a.labels else [Path(n).stem for n in a.nros]
    _, nros = run_ab(con, a, a.nros, names)
    if a.final:
        con.deploy(nros.get(a.final, a.final))
        log(f"left the console on {a.final}: {con.info().get('version')}")


def cmd_compare_to(a):
    """the contributor's build (B) against a baseline (A): main or the last published tag"""
    if platform.system() == "Windows" or platform.system().startswith(("MINGW", "MSYS", "CYGWIN")):
        raise SystemExit("compare-to runs on macOS and Linux with the container build (docs/perf-test.md); "
                         "on Windows, build both NROs yourself and use perf_test.py ab")
    con = Console(a.host)
    if con.try_cmd("ping", timeout=5.0) is None:
        raise SystemExit(f"the console at {a.host} does not answer: start the game with the debug server on "
                         "(Switch tab, Debug section), or give --host")
    repo = perf_baseline.REPO
    perf_baseline.fetch(repo)
    base_nro, base_name = perf_baseline.build(repo, a.baseline, a.rpx)
    if a.nro:
        mine = Path(a.nro).resolve()
    else:
        log("building your checkout (tools/switch/build.sh)")
        subprocess.run(["tools/switch/build.sh"], cwd=repo, check=True)
        mine = repo / "build" / "switch-dk" / "wwhd.nro"
    if not mine.is_file():
        raise SystemExit(f"no NRO: {mine}")
    names = [base_name.replace("/", "-"), a.label]
    try:
        results, _ = run_ab(con, a, [base_nro, mine], names)
    finally:
        # the console is left on the contributor's build, also after Ctrl+C or an error
        try:
            con.deploy(str(mine))
            log(f"left the console on your build: {con.info().get('version')}")
        except (SystemExit, OSError) as e:
            log(f"could not put your build back on the console: {e}")
        con.try_cmd("bench", "clock", "run")
    print()
    print(f"Verdict, {a.label} against {base_name} ({len(a.order) * a.repeat // 2} boots each, "
          "95% interval of the run spread):")
    for line in verdict(results, names[0], names[1]):
        print("  " + line)


def record(con, frames):
    kills = LogCounter(con, "kill DSP channel")
    text = con.cmd("bench", "rec", str(frames), timeout=frames / 10 + 60)
    dsp_kills = kills.finish()
    rows, no_control = [], 0
    for line in text.splitlines():
        if line.startswith("# no_control_checks"):
            no_control = int(line.split()[-1])
        elif line and not line.startswith("#"):
            rows.append([int(x) for x in line.split()])
    res = summarise(rows, no_control)
    res["dsp_kills"] = dsp_kills
    return res, rows


def cmd_toggle(a):
    """A/B inside one boot: a debug-server switch (e.g. psmtx 0 / psmtx 1) flipped between recordings of the same
    scene, so the per-boot spread does not enter the comparison"""
    con = Console(a.host)
    scenes = scenes_arg(a)
    cmds = {"A": a.cmd_a.split(), "B": a.cmd_b.split()}
    labels = {"A": a.label_a, "B": a.label_b}
    ensure_states(con, scenes)
    if a.nro:
        con.deploy(a.nro)
    info = con.info()
    log(f"console: {info.get('version')}")
    boot_to_gameplay(con, reload=not a.no_boot and not a.nro)
    results, raw = [], []
    for name in scenes:
        con.cmd("bench", "clock", "freeze", f"{a.time:.2f}")
        arrivals = int(con.status().get("arrivals", "0"))
        con.cmd("bench", "load", name)
        end = time.time() + 90
        while time.time() < end and int(con.status().get("arrivals", "0")) <= arrivals:
            time.sleep(1.0)
        time.sleep(a.settle)
        for i, letter in enumerate(a.order * a.repeat):
            log(f"{name}: {' '.join(cmds[letter])} -> {con.cmd(*cmds[letter]).strip()}")
            time.sleep(a.switch_settle)
            res, rows = record(con, a.frames)
            res.update({"build": labels[letter], "scene": name, "run": i, "stage": con.status().get("stage")})
            results.append(res)
            raw.append({"build": labels[letter], "scene": name, "rows": rows})
            log(f"[{labels[letter]}] {name:12s} {res['fps']:5.2f} fps  main {res['main_ms']:5.2f} ms  render "
                f"{res['render_ms']:5.2f} ms  gpu {res['gpu_ms'] or 0:5.1f} ms  dsp kills {res['dsp_kills']}"
                + (f"  NO CONTROL {res['no_control']}" if res["no_control"] else ""))
    con.try_cmd(*cmds["A"])
    con.try_cmd("bench", "clock", "run")
    print(table(results, [a.label_a, a.label_b]))
    save(results, raw, f"toggle_{a.label_a}_vs_{a.label_b}",
         {"version": info.get("version"), "args": {k: v for k, v in vars(a).items() if k != "fn"}})


def get_text(con, remote):
    c = dbg.Conn(con.host, con.port, timeout=10.0)
    try:
        return c.command("get", remote, timeout=60.0).decode(errors="replace")
    finally:
        c.close()


def settings_with_profile(text, profile):
    """the player's settings.ini with switchGpuProfile set to profile"""
    lines = [line for line in text.splitlines() if not line.startswith("switchGpuProfile=")]
    # before a [dev] section, if any: the key belongs to the main part
    at = next((i for i, line in enumerate(lines) if line.strip() == "[dev]"), len(lines))
    lines.insert(at, f"switchGpuProfile={profile}")
    return "\n".join(lines) + "\n"


def boot_clocks(con):
    """(CPU, GPU, memory) MHz of the running boot's last '[dk] clocks' line, and its last per-core load"""
    text = con.cmd("logtext", timeout=60.0)
    clocks = re.findall(r"\[dk\] clocks: CPU (\d+) MHz, GPU (\d+) MHz, memory (\d+) MHz", text)
    cores = re.findall(r"\[cpu\] cores busy \(since the last report\): core 0 (\d+)%, core 1 (\d+)%, core 2 (\d+)%", text)
    return (tuple(int(x) for x in clocks[-1]) if clocks else None), (tuple(int(x) for x in cores[-1]) if cores else None)


def cmd_gpu_profile(a):
    """one build, the GPU profile alternated between boots: is a view GPU-bound, and what a profile gains there"""
    con = Console(a.host)
    profiles = a.profiles.split(",")
    for prof in profiles:
        if prof not in GPU_PROFILES:
            raise SystemExit(f"unknown profile {prof}: one of {', '.join(GPU_PROFILES)}")
    letters = "ABCD"[:len(profiles)]
    names = dict(zip(letters, profiles))
    order = (a.order or (letters + letters[::-1])) * a.repeat
    if set(order) - set(letters):
        raise SystemExit(f"--order uses letters {letters} only")
    scenes = scenes_arg(a)
    ensure_states(con, scenes)
    info = con.info()
    log(f"console: {info.get('version')}")
    original = get_text(con, "settings.ini")
    OUT.mkdir(parents=True, exist_ok=True)
    backup = OUT / f"settings.ini.backup_{time.strftime('%Y%m%d_%H%M%S')}"
    backup.write_text(original)
    log(f"the console's settings.ini is kept in {backup} and put back at the end")
    stem_name = "gpu_" + "_vs_".join(profiles)
    results, raw = [], []
    try:
        for i, letter in enumerate(order):
            prof = names[letter]
            log(f"[{i + 1}/{len(order)}] GPU profile {prof}")
            tmp = OUT / "settings.ini.run"
            tmp.write_text(settings_with_profile(original, prof))
            con.put(tmp, "settings.ini")
            boot_to_gameplay(con, reload=True)
            clocks, _ = boot_clocks(con)
            gpu, mem = GPU_PROFILES[prof]
            if not clocks or clocks[1] != gpu or clocks[2] != mem:
                raise SystemExit(f"profile {prof} should give GPU {gpu} / memory {mem} MHz, the log says {clocks} "
                                 "(sys-clk on? the game started from hbmenu, where apm is skipped?)")
            n = len(results)
            run_set(con, prof, scenes, 1, a.settle, a.frames, a.time, results, raw, rotate_from=i, warmup=a.warmup)
            clocks, cores = boot_clocks(con)
            for r in results[n:]:
                r.update({"clocks": clocks, "cores": cores})
            log(f"  clocks {clocks}, cores busy {cores} (core 1: the main thread; core 2 or 0: the render thread)")
            save(results, raw, stem_name + "_partial", {"order": order, "profiles": profiles})
    finally:
        log("putting the console's settings.ini back")
        con.put(backup, "settings.ini")
        con.try_cmd("reload", timeout=10.0)
    print(table(results, profiles))
    for prof in profiles[1:]:
        print(f"\n{prof} against {profiles[0]} (95% interval of the boot-to-boot spread):")
        for line in verdict(results, profiles[0], prof):
            print("  " + line)
    save(results, raw, stem_name, {"version": info.get("version"), "order": order,
                                   "args": {k: v for k, v in vars(a).items() if k != "fn"}})


def load(path):
    return json.loads(Path(path).read_text())["results"]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("run")
    common(sp)
    sp.add_argument("--nro", help="deploy this NRO first")
    sp.add_argument("--label")
    sp.add_argument("--runs", type=int, default=1)
    sp.set_defaults(fn=cmd_run)
    sp = sub.add_parser("ab")
    common(sp)
    sp.add_argument("nros", nargs="+", help="2 or more NROs: A, B, C...")
    sp.add_argument("--labels", help="comma-separated names (default: the NRO file names)")
    sp.add_argument("--order", help="letters, e.g. ABBA (default: A..Z then Z..A, e.g. ABCCBA)")
    sp.add_argument("--repeat", type=int, default=1)
    sp.add_argument("--final", default="A", help="a letter, an NRO path, or '' to leave the last one")
    sp.set_defaults(fn=cmd_ab)
    sp = sub.add_parser("compare-to", description="your build against main or the last published tag; its own "
                        "defaults: --scenes lava,windfall,dragonroost --frames 900 --order ABBA --repeat 2")
    common(sp)
    sp.add_argument("baseline", help="main, last-tag (the newest tag on origin/main), or a tag")
    sp.add_argument("--nro", help="your NRO (default: build your checkout with tools/switch/build.sh)")
    sp.add_argument("--rpx", help="your cking.rpx, to generate the baseline's code once (or WWHD_RPX)")
    sp.add_argument("--label", default="mine")
    sp.add_argument("--order", default="ABBA")
    sp.add_argument("--repeat", type=int, default=2)
    sp.set_defaults(fn=cmd_compare_to, scenes="lava,windfall,dragonroost", frames=900)
    sp = sub.add_parser("toggle")
    common(sp)
    sp.add_argument("--nro", help="deploy this NRO first")
    sp.add_argument("--cmd-a", required=True, help='debug command for A, e.g. "psmtx 0"')
    sp.add_argument("--cmd-b", required=True, help='debug command for B, e.g. "psmtx 1"')
    sp.add_argument("--label-a", default="off")
    sp.add_argument("--label-b", default="on")
    sp.add_argument("--order", default="ABBA")
    sp.add_argument("--repeat", type=int, default=3)
    sp.add_argument("--switch-settle", type=float, default=3.0, help="seconds after each switch (default 3)")
    sp.set_defaults(fn=cmd_toggle)
    sp = sub.add_parser("gpu-profile", description="the handheld GPU profile alternated between boots of the build "
                        "on the console; its own defaults: --scenes outset_tallgrass --time 150 --order ABBA --repeat 2")
    common(sp)
    sp.add_argument("--profiles", default="307,384",
                    help="comma-separated switchGpuProfile values, A first: " + ", ".join(GPU_PROFILES)
                    + " (default: 307,384)")
    sp.add_argument("--order", default="ABBA")
    sp.add_argument("--repeat", type=int, default=2)
    sp.set_defaults(fn=cmd_gpu_profile, scenes=GPU_SCENE, time=GPU_SCENE_TIME)
    sp = sub.add_parser("compare")
    sp.add_argument("files", nargs="+")
    sp.set_defaults(fn=lambda a: print(table([r for f in a.files for r in load(f)])))
    sp = sub.add_parser("show")
    sp.add_argument("file")
    sp.set_defaults(fn=lambda a: print(table(load(a.file))))
    a = p.parse_args()
    global QUEST_LOG
    QUEST_LOG = getattr(a, "quest_log", 1)
    a.fn(a)


if __name__ == "__main__":
    main()
