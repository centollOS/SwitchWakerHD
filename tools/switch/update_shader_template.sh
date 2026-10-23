#!/usr/bin/env bash
# Maintainers (issue #88, docs/shader-manifest-release-plan.md "Updating"): after the test console has played, check
# its shader recording for shaders the release's template does not have yet, and if there are any, make the new
# sealed template in the private repository (centollOS/SwitchWakerHD-shader-manifest).
#
#   tools/switch/update_shader_template.sh GAME_DIR [MANIFEST]
#
# GAME_DIR: the extracted game (USA or European, version 0). MANIFEST: the console's shader_manifest.bin; by default
# the SD card's when it is mounted (hekate's USB mass storage, "/Volumes/SWITCH SD"), else fetched over the debug
# server (tools/switch/wwhd_debug.py: the game running with it on). WWHD_SHADER_TEMPLATE_REPO: the private
# repository's clone (default ../SwitchWakerHD-shader-manifest next to this checkout).
#
# Nothing is committed or pushed: when there is something new it says what to do next.
set -euo pipefail
cd "$(dirname "$0")/../.."
[[ $# -ge 1 && $# -le 2 ]] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
game=$1
repo=${WWHD_SHADER_TEMPLATE_REPO:-../SwitchWakerHD-shader-manifest}
work=build/shader-template
mkdir -p "$work"
[[ -f "$repo/shader-template.sealed" ]] || { echo "no $repo/shader-template.sealed: clone the private repository there" >&2; exit 1; }
git -C "$repo" pull -q --ff-only
manifest=${2:-}
if [[ -z "$manifest" ]]; then
    if [[ -f "/Volumes/SWITCH SD/switch/wwhd/shader_manifest.bin" ]]; then
        manifest="/Volumes/SWITCH SD/switch/wwhd/shader_manifest.bin"
    else
        manifest="$work/console_manifest.bin"
        python3 tools/switch/wwhd_debug.py get /switch/wwhd/shader_manifest.bin "$manifest"
    fi
fi
echo "recording: $manifest"
set +e
python3 tools/switch/shader_manifest.py distill "$manifest" "$game" "$work/template.bin" \
    --base "$repo/shader-template.sealed" --work "$work/distill"
rc=$?
set -e
if [[ $rc == 3 ]]; then
    echo "nothing new: the template already has every shader of this recording"
    exit 0
fi
[[ $rc == 0 ]] || exit $rc
python3 tools/switch/shader_manifest.py seal "$work/template.bin" "$game" "$repo/shader-template.sealed"
python3 - "$repo" "$work/template.bin" <<'PY'
import datetime, hashlib, json, os, subprocess, sys
repo, plain = sys.argv[1], sys.argv[2]
sys.path.insert(0, "tools")
import shaderprep_format as tf
states = tf.decode(open(plain, "rb").read())["states"]
path = os.path.join(repo, "provenance.json")
p = json.load(open(path))
p.update(sha256=hashlib.sha256(open(os.path.join(repo, "shader-template.sealed"), "rb").read()).hexdigest(),
         made=datetime.date.today().isoformat(), states=len(states), vertex_states=sum(s["vertex"] for s in states),
         pixel_states=sum(not s["vertex"] for s in states),
         distilled_by="SwitchWakerHD " + subprocess.check_output(["git", "rev-parse", "--short=8", "HEAD"], text=True).strip())
p["from"] = p["from"] + "; updated with a later recording"
json.dump(p, open(path, "w"), indent=2)
open(path, "a").write("\n")
print("provenance.json: %d states, sha256 %s" % (len(states), p["sha256"]))
PY
cat <<MSG
Next:
  1. git -C $repo commit -am "Template: shaders of a later recording" && git -C $repo push
  2. tools/shadercache/manifest.ref here: the private commit and the sha256 above (the next release attaches it)
MSG
