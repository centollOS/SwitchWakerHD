#!/usr/bin/env python3
"""The game's data files (content/) against the list of each supported build (issue #48).

An incomplete dump, a copy cut off or a damaged file shows up only as a crash somewhere in the game (#46): each
build's list, tools/recomp/builds/<build>-files.txt, holds every file of content/ with its size and SHA-256
(names, sizes and hashes only, never game data). make_sd.py checks the game against it before building.

usage:
  game_files.py check GAME_DIR [BUILD]   compare a game folder with its build's list (BUILD: USA, EU; default:
                                         from code/cking.rpx)
  game_files.py make GAME_DIR BUILD      write BUILD's list from a known-good game folder
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LISTS = os.path.join(HERE, "..", "recomp", "builds")


def list_path(build):
    return os.path.join(LISTS, "%s-files.txt" % build)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def game_files(game):
    """content/'s files, as 'content/...' paths with '/'"""
    out = []
    for dp, dirs, fns in os.walk(os.path.join(game, "content")):
        dirs.sort()
        for fn in sorted(fns):
            out.append(os.path.relpath(os.path.join(dp, fn), game).replace(os.sep, "/"))
    return out


def read_list(build):
    """{path: (size, sha256)}, or None when the build has no list"""
    try:
        with open(list_path(build), encoding="utf-8") as f:
            lines = [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]
    except FileNotFoundError:
        return None
    expected = {}
    for line in lines:
        digest, size, path = line.split("\t")
        expected[path] = (int(size), digest)
    return expected


def check(game, build):
    """(missing, wrong size, wrong content) paths, or None when the build has no list. Files the list does not
    know (left by a dump tool, say) do not count."""
    expected = read_list(build)
    if expected is None:
        return None
    missing, size, content = [], [], []
    for path, (want_size, want_digest) in sorted(expected.items()):
        full = os.path.join(game, *path.split("/"))
        try:
            have = os.path.getsize(full)
        except OSError:
            missing.append(path)
            continue
        if have != want_size:
            size.append(path)
        elif sha256(full) != want_digest:
            content.append(path)
    return missing, size, content


def report(problems, limit=20):
    """the lines that tell which files are wrong, at most limit of each kind"""
    lines = []
    for what, paths in zip(("missing", "wrong size (cut off?)", "damaged (same size, other content)"), problems):
        for p in paths[:limit]:
            lines.append("%s: %s" % (what, p))
        if len(paths) > limit:
            lines.append("%s: ... and %d more" % (what, len(paths) - limit))
    return lines


def make(game, build):
    files = game_files(game)
    with open(list_path(build), "w", encoding="utf-8", newline="\n") as f:
        f.write("# The Wind Waker HD (%s), version 0: content/'s files (tools/switch/game_files.py)\n" % build)
        f.write("# sha256\tsize\tpath\n")
        for path in files:
            full = os.path.join(game, *path.split("/"))
            f.write("%s\t%d\t%s\n" % (sha256(full), os.path.getsize(full), path))
    print("%s: %d files" % (list_path(build), len(files)))


def main(argv):
    if len(argv) >= 2 and argv[0] == "make" and len(argv) == 3:
        make(argv[1], argv[2])
        return 0
    if len(argv) in (2, 3) and argv[0] == "check":
        game = argv[1]
        if len(argv) == 3:
            build = argv[2]
        else:
            sys.path.insert(0, os.path.join(HERE, "..", "installer"))
            import setup
            build = setup.check_game_version(game).name
        problems = check(game, build)
        if problems is None:
            print("no list for %s" % build)
            return 2
        for line in report(problems):
            print(line)
        bad = sum(len(p) for p in problems)
        print("%s: %s" % (build, "%d files wrong" % bad if bad else "all files OK"))
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
