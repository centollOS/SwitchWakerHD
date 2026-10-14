#!/usr/bin/env python3
"""snan_merge.py [LOG...]: combine the sessions of captures/snan.log files (WWHD_SNAN_COUNT=1, see
runtime/src/snan_count.cpp) into one table: kind, site (game function + host offset), total count,
sessions seen, first guest address / bit pattern / stage. Default LOG: captures/snan.log.

A session that ended without its summary (crash, kill) uses its last 30-second snapshot, else counts
each of its HIT sites once."""
import re
import sys

HIT = re.compile(r"^HIT (\S+) (\S+) ea=(\S+) bits=(\S+) stage=(\S*)")
SITE = re.compile(r"^(SITE|SNAP) (\S+) (\S+) count=(\d+)")


def main():
    logs = sys.argv[1:] or ["captures/snan.log"]
    sites = {}
    sessions = ended = 0
    for path in logs:
        cur = None
        for line in open(path, errors="replace"):
            line = line.rstrip("\n")
            if line.startswith("# session "):
                if cur is not None:
                    close(sites, cur)
                sessions += 1
                cur = {"hits": {}, "counts": {}, "snap": {}, "name": line[10:]}
            elif cur is None:
                continue
            elif m := HIT.match(line):
                cur["hits"][(m[1], m[2])] = (m[3], m[4], m[5])
            elif line.startswith("# snapshot "):
                cur["snap"] = {}
            elif m := SITE.match(line):
                cur["counts" if m[1] == "SITE" else "snap"][(m[2], m[3])] = int(m[4])
            elif line.startswith("# end "):
                ended += 1
                close(sites, cur)
                cur = None
        if cur is not None:
            close(sites, cur)
    print("sessions %d (%d with a summary), sites %d, hits %d" % (
        sessions, ended, len(sites), sum(s["count"] for s in sites.values())))
    if not sites:
        print("no signalling NaN met by a float load/store")
        return
    print("%-7s %-34s %12s %8s  %s" % ("kind", "site", "count", "sessions", "first ea / bits / stage"))
    for (kind, site), s in sorted(sites.items(), key=lambda kv: -kv[1]["count"]):
        print("%-7s %-34s %12d %8d  %s %s %s" % (kind, site, s["count"], s["sessions"], *s["first"]))


def close(sites, cur):
    if not cur["counts"]:
        cur["counts"] = cur["snap"]
    keys = set(cur["hits"]) | set(cur["counts"])
    for k in keys:
        s = sites.setdefault(k, {"count": 0, "sessions": 0, "first": cur["hits"].get(k, ("?", "?", "?"))})
        s["count"] += cur["counts"].get(k, 1)
        s["sessions"] += 1


if __name__ == "__main__":
    main()
