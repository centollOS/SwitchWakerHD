#!/usr/bin/env python3
"""Replays an image heap trace (WWHD_DK_IMAGE_TRACE=1, image_trace.bin; runtime/src/gfx/deko/memory.cpp) with
other placement policies and reports the chunks each one needs (docs/memory-round2-plan.md, step 2).

The placements and frees are the session's own, evictions included: a policy that would have needed fewer
evictions is not credited for it. A policy adds a 64 MiB chunk (bigger for a bigger image) only when nothing fits.

usage: image_heap_sim.py image_trace.bin [--policy NAME ...] [--big MiB]"""
import argparse
import bisect
import struct
import sys

CHUNK = 64 << 20
ALLOC, FREE, PRESSURE, NEW_CHUNK = 1, 2, 3, 4


def read_trace(path):
    data = open(path, "rb").read()
    if data[:4] != b"WIT1":
        sys.exit(f"{path}: not an image heap trace (no WIT1 magic)")
    (size,) = struct.unpack_from("<I", data, 4)
    if size != 16:
        sys.exit(f"{path}: records of {size} bytes, this tool reads 16")
    end = 8 + (len(data) - 8) // 16 * 16
    return list(struct.iter_unpack("<BBHIII", data[8:end]))


def align_up(v, a):
    return (v + a - 1) // a * a


class Heap:
    """Chunks with free lists sorted by offset (as memory.cpp)."""

    def __init__(self, policy, big):
        self.policy, self.big = policy, big
        self.chunks = []  # [size, [(offset, length)...]]
        self.peak_used = self.used = 0

    def add_chunk(self, size):
        self.chunks.append([size, [(0, size)]])

    def insert(self, ci, off, size):
        """A range back to a chunk's free list, merged with its neighbours (memory.cpp chunk_free)."""
        fl = self.chunks[ci][1]
        i = bisect.bisect_left(fl, (off, 0))
        fl.insert(i, (off, size))
        if i + 1 < len(fl) and fl[i][0] + fl[i][1] == fl[i + 1][0]:
            fl[i] = (fl[i][0], fl[i][1] + fl[i + 1][1])
            del fl[i + 1]
        if i > 0 and fl[i - 1][0] + fl[i - 1][1] == fl[i][0]:
            fl[i - 1] = (fl[i - 1][0], fl[i - 1][1] + fl[i][1])
            del fl[i]

    def free(self, ci, off, size):
        self.insert(ci, off, size)
        self.used -= size

    def place(self, size, align):
        """(chunk, offset) of a free range per the policy, or None. first: the first fit, chunks in order (as
        memory.cpp); best: the smallest range it fits; split / split-best: images of `big` bytes or more go to the
        top (the end of the last range they fit, so they gather at the top of the last chunks) and the rest as first
        / best."""
        top = self.policy.startswith("split") and size >= self.big
        bestfit = self.policy in ("best", "split-best")
        pick = None  # (sort key, chunk, index, range offset, range length, start)
        for ci, (_, fl) in enumerate(self.chunks):
            for i, (off, ln) in enumerate(fl):
                at = (off + ln - size) // align * align if top else align_up(off, align)
                if at < off or at + size > off + ln:
                    continue
                if bestfit:
                    key = (ln, -ci, -off) if top else (ln, ci, off)
                elif top:
                    key = (-ci, -off)  # the highest fit
                else:
                    key = (ci, off)  # the first fit
                if pick is None or key < pick[0]:
                    pick = (key, ci, i, off, ln, at)
                if not bestfit and not top:
                    break
            if pick is not None and not bestfit and not top:
                break
        if pick is None:
            return None
        _, ci, i, off, ln, at = pick
        del self.chunks[ci][1][i]
        if at > off:
            self.insert(ci, off, at - off)
        if at + size < off + ln:
            self.insert(ci, at + size, off + ln - at - size)
        self.used += size
        self.peak_used = max(self.peak_used, self.used)
        return ci, at

    def alloc(self, size, align):
        while True:
            r = self.place(size, align)
            if r is not None:
                return r
            self.add_chunk(max(CHUNK, align_up(size + align, 1 << 12)))

    def largest_free(self):
        return max((ln for _, fl in self.chunks for _, ln in fl), default=0)


def run(events, policy, big):
    h = Heap(policy, big)
    h.add_chunk(CHUNK)
    live = {}
    first_chunk_frame = {}
    for kind, align_log2, _, frame, ident, size in events:
        if kind == ALLOC:
            live[ident] = (*h.alloc(size, 1 << align_log2), size)
            first_chunk_frame.setdefault(len(h.chunks), frame)
        elif kind == FREE and ident in live:
            ci, at, sz = live.pop(ident)
            h.free(ci, at, sz)
    return h, first_chunk_frame


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--policy", action="append", choices=["first", "best", "split", "split-best"])
    ap.add_argument("--big", type=float, default=1.0, help="split policies: images of this many MiB or more go on top")
    a = ap.parse_args()
    events = read_trace(a.trace)
    allocs = sum(1 for e in events if e[0] == ALLOC)
    recorded = 1 + sum(1 for e in events if e[0] == NEW_CHUNK and e[4] > 0)
    print(f"{len(events)} events, {allocs} placements, {recorded} chunks in the session")
    for policy in a.policy or ["first", "best", "split", "split-best"]:
        h, firsts = run(events, policy, int(a.big * (1 << 20)))
        live = h.used / (1 << 20)
        total = sum(c[0] for c in h.chunks) / (1 << 20)
        print(f"  {policy:10s} chunks {len(h.chunks)} ({total:.0f} MiB), at the end {live:.0f} MiB placed, "
              f"peak {h.peak_used / (1 << 20):.0f} MiB placed, largest free {h.largest_free() / (1 << 20):.1f} MiB; chunk n first needed at frame "
              + ", ".join(f"{n}:{f}" for n, f in sorted(firsts.items()) if n > 1))


if __name__ == "__main__":
    main()
