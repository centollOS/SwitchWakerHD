#!/usr/bin/env python3
"""pgo_pack.py with made-up .gcda files: a round trip, and the zips it refuses."""
import json
import os
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pgo_pack  # noqa: E402

GCDA = {"#CMakeFiles#gamecode.dir#build#gen#code_hot_000.gcda": b"adcg" + b"\1" * 60,
        "#CMakeFiles#wwhd.dir#runtime#src#gfx#deko#backend.gcda": b"adcg" + b"\2" * 90}
INFO = {"compiler": "GNU 16.1.0", "scope": "game;runtime", "hot_sha256": "ab" * 32, "players_code": True}


def refuses(zip_path, out, why, region=None):
    try:
        pgo_pack.unpack(zip_path, out, region)
    except pgo_pack.Fail as e:
        assert why in str(e), "expected %r, got %r" % (why, str(e))
        return
    raise AssertionError("accepted a zip it should refuse (%s)" % why)


def rewrite(src, dst, change):
    """a copy of the zip src with change(name, data) -> (name, data) or None applied to every member"""
    with zipfile.ZipFile(src) as z, zipfile.ZipFile(dst, "w") as o:
        for i in z.infolist():
            r = change(i.filename, z.read(i.filename))
            if r:
                o.writestr(r[0], r[1])


def main():
    with tempfile.TemporaryDirectory() as t:
        gcda = os.path.join(t, "pgo")
        build = os.path.join(t, "build")
        os.makedirs(gcda)
        os.makedirs(build)
        for n, d in GCDA.items():
            with open(os.path.join(gcda, n), "wb") as f:
                f.write(d)
        with open(os.path.join(build, "pgo-build.json"), "w") as f:
            json.dump(INFO, f)

        z = pgo_pack.pack(gcda, build, "USA", t)
        assert os.path.basename(z) == "pgo-profile-USA.zip"
        with open(z, "rb") as f:
            first = f.read()
        with open(z + ".sha256") as f:
            assert f.read().split()[0] == pgo_pack.sha256(first)
        pgo_pack.pack(gcda, build, "USA", t)  # the same input gives the same zip
        with open(z, "rb") as f:
            assert f.read() == first, "the zip is not reproducible"

        out = os.path.join(t, "out")
        os.makedirs(out)
        with open(os.path.join(out, "old.gcda"), "wb") as f:  # an earlier profile's file is removed
            f.write(b"x")
        m = pgo_pack.unpack(z, out, "USA")
        assert m["region"] == "USA" and m["compiler"] == INFO["compiler"] and m["scope"] == INFO["scope"]
        assert m["hot_sha256"] == INFO["hot_sha256"]
        assert sorted(os.listdir(out)) == sorted(list(GCDA) + ["manifest.json"])
        for n, d in GCDA.items():
            with open(os.path.join(out, n), "rb") as f:
                assert f.read() == d

        refuses(z, out, "the profile is for the USA game", region="EU")
        bad = os.path.join(t, "bad.zip")
        rewrite(z, bad, lambda n, d: ("../evil.gcda", d) if n.endswith("code_hot_000.gcda") else (n, d))
        refuses(bad, out, "unexpected file")
        rewrite(z, bad, lambda n, d: (n, d + b"!") if n.endswith(".gcda") else (n, d))
        refuses(bad, out, "does not match its SHA-256")
        rewrite(z, bad, lambda n, d: None if n == "manifest.json" else (n, d))
        refuses(bad, out, "no manifest.json")
        rewrite(z, bad, lambda n, d: None if n.endswith("backend.gcda") else (n, d))
        refuses(bad, out, "file list does not match")
        rewrite(z, bad, lambda n, d: (n, d.replace(b'"format": 1', b'"format": 9')) if n == "manifest.json" else (n, d))
        refuses(bad, out, "profile format 9")
        with open(bad, "wb") as f:
            f.write(b"not a zip")
        refuses(bad, out, "not a profile zip")

        try:
            pgo_pack.pack(os.path.join(t, "nothing"), build, "USA", t)
            raise AssertionError("packed an empty folder")
        except pgo_pack.Fail as e:
            assert "no .gcda files" in str(e)

        # #130: a training build on code that is not the players' is refused, unless asked for
        with open(os.path.join(build, "pgo-build.json"), "w") as f:
            json.dump(dict(INFO, players_code=False), f)
        try:
            pgo_pack.pack(gcda, build, "EU", t)
            raise AssertionError("packed a profile trained on code no player builds")
        except pgo_pack.Fail as e:
            assert "not the players'" in str(e)
        pgo_pack.pack(gcda, build, "EU", t, other_code=True)
        with open(os.path.join(build, "pgo-build.json"), "w") as f:  # an older build folder: packed, with a warning
            json.dump({k: v for k, v in INFO.items() if k != "players_code"}, f)
        pgo_pack.pack(gcda, build, "EU", t)
    print("test_pgo_pack: OK")


if __name__ == "__main__":
    main()
