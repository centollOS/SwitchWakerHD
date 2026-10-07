#!/usr/bin/env python3
"""Release guard: fails if a release artifact contains anything derived from the game or any key.

usage: guard.py ARTIFACT.zip|DIR [...]

Release packages may contain only this project's runtime, tools and installer (plus third-party
licenses). This check rejects, by name and by content:
  - generated/recompiled game code (code_NNN.c, funcs.h with guest functions, table.c, imports.c,
    imports.json, report.txt from tools/recomp), and real-looking generated code inside any file;
  - game files: *.rpx, *.rpl, *.wux, *.wud, disc keys (*.key), tickets/TMDs (title.tik, title.tmd),
    saves (cking*.sav, *.sav), anything under a game/ code/ content/ meta/ tree;
  - shader caches and head starts (shaders.bin, headstart.bin, template.bin, testcache.bin, *.metallib);
  - in every text file: a 32-hex-digit string (the shape of a Wii U key; SHA-256 sums are 64 digits
    and git hashes 40, which do not match).
Exit status 1 lists every problem.
"""
import os
import re
import sys
import zipfile

BAD_NAME = [
    (re.compile(r"(^|/)code_\d+\.c$"), "generated game code"),
    (re.compile(r"(^|/)(table|imports)\.c$"), "generated game code tables"),
    (re.compile(r"(^|/)imports\.json$"), "generated import table"),
    (re.compile(r"(^|/)funcs\.h$"), "generated function list"),
    (re.compile(r"\.(rpx|rpl|wux|wud|iso|wua|rvz|gci)$", re.I), "game executable or disc image"),
    (re.compile(r"\.key$", re.I), "key file"),
    (re.compile(r"(^|/)title\.(tik|tmd|cert)$", re.I), "disc ticket/metadata"),
    (re.compile(r"\.sav$", re.I), "save file"),
    (re.compile(r"(^|/)(shaders|headstart|template|testcache|mycache)\.bin$", re.I), "shader cache"),
    (re.compile(r"\.metallib$", re.I), "compiled shader cache"),
    (re.compile(r"(^|/)(game|content|meta)/"), "game file tree"),
    (re.compile(r"(^|/)build/gen"), "recompiler output"),
    (re.compile(r"(^|/)(lib)?gamecode\.(a|lib)$"), "compiled game code"),
    (re.compile(r"(^|/)common\.key$", re.I), "Wii U common key"),
]
TEXT_EXT = {".py", ".txt", ".md", ".json", ".sh", ".command", ".bat", ".ps1", ".h", ".hpp", ".c", ".cpp", ".inl",
            ".cfg", ".ini", ".xml", ".plist", ".toml", ".yml", ".yaml", ".rsp", ""}
KEYLIKE = re.compile(rb"(?<![0-9A-Fa-f])[0-9A-Fa-f]{32}(?![0-9A-Fa-f])")
# a function body as tools/recomp/recomp.py emits it (stubgen placeholders call ppc_unimplemented)
GEN_CODE = re.compile(rb"void f_[0-9A-F]{8}\(Cpu\* __restrict c\) \{\n")


# vendored third-party sources may use the generated code's file names (uam's Mesa has a main/imports.c):
# the name rules for generated code skip them; every content check still applies
VENDORED = re.compile(r"(^|/)runtime/third_party/")


def check_entry(name, data, problems):
    n = name.replace("\\", "/")
    vendored = VENDORED.search(n)
    for rx, why in BAD_NAME:
        if vendored and why.startswith("generated"):
            continue
        if rx.search(n):
            problems.append("%s: %s" % (name, why))
    ext = os.path.splitext(n)[1].lower()
    if ext in TEXT_EXT and b"\0" not in data[:8192]:
        for m in KEYLIKE.finditer(data):
            line = data.count(b"\n", 0, m.start()) + 1
            problems.append("%s:%d: 32-hex-digit string (key-like)" % (name, line))
    if GEN_CODE.search(data):
        problems.append("%s: contains recompiled game functions" % name)


def scan(path):
    problems, count = [], 0
    if os.path.isdir(path):
        for dp, _, fns in os.walk(path):
            for fn in fns:
                full = os.path.join(dp, fn)
                with open(full, "rb") as f:
                    check_entry(os.path.relpath(full, path), f.read(), problems)
                count += 1
    else:
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                check_entry(info.filename, z.read(info), problems)
                count += 1
    return problems, count


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bad = False
    for p in sys.argv[1:]:
        problems, count = scan(p)
        if problems:
            bad = True
            print("REJECTED %s (%d files):" % (p, count))
            for pr in problems:
                print("  " + pr)
        else:
            print("ok %s (%d files checked)" % (p, count))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
