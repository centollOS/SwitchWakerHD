#!/usr/bin/env bash
# Package a SwitchWakerHD release: the source of a git tag (or commit) as a zip, with INSTALL.md at its top,
# checked by tools/release/guard.py (no game code, game files, shader caches or keys). There is no NRO in
# it: every build carries the game's recompiled code, so players build their own (tools/switch/make_sd.py).
#   tools/switch/package_release.sh v0.2.0    -> build/release/SwitchWakerHD-v0.2.0.zip and .sha256
set -euo pipefail
cd "$(dirname "$0")/../.."
ref=${1:?usage: package_release.sh TAG_OR_COMMIT}
name=SwitchWakerHD-${ref}
out=build/release
mkdir -p "$out"
rm -f "$out/$name.zip"
git archive --format=zip --prefix="$name/" -o "$out/$name.zip" "$ref"
python3 -I tools/release/guard.py "$out/$name.zip"
(cd "$out" && shasum -a 256 "$name.zip" > "$name.zip.sha256" && cat "$name.zip.sha256")
