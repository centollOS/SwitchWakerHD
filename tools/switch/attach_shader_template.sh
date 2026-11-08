#!/usr/bin/env bash
# Maintainers (issue #88, docs/shader-manifest-release-plan.md): the release's sealed shader template, kept in the
# private repository centollOS/SwitchWakerHD-shader-manifest and pinned here in tools/switch/shader_template.ref.
#
#   tools/switch/attach_shader_template.sh pin
#       the pin from the private clone's HEAD (after update_shader_template.sh made a new template and it was pushed)
#   tools/switch/attach_shader_template.sh TAG [GAME_DIR]
#       the pinned file from the private repository (the clone fetched), checked against the pin's SHA-256 (and, with
#       GAME_DIR, opened with that dump), uploaded to the GitHub release TAG as shader-template.sealed and its
#       .sha256, which the installers download (installer/shared/EngineBase.cs ShaderTemplate)
#
# WWHD_SHADER_TEMPLATE_REPO: the private repository's clone (default ../SwitchWakerHD-shader-manifest next to this
# checkout; cloned there with gh when missing). Players and forks have no access: the source zip never holds the file
# (tools/release/guard.py refuses it).
set -euo pipefail
cd "$(dirname "$0")/../.."
[[ $# -ge 1 && $# -le 2 ]] || { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
repo=${WWHD_SHADER_TEMPLATE_REPO:-../SwitchWakerHD-shader-manifest}
ref=tools/switch/shader_template.ref
[[ -d "$repo/.git" ]] || gh repo clone centollOS/SwitchWakerHD-shader-manifest "$repo"
git -C "$repo" fetch -q origin
field() { python3 -c "import json,sys; print(json.load(open('$ref'))['$1'])"; }

if [[ $1 == pin ]]; then
    git -C "$repo" pull -q --ff-only
    commit=$(git -C "$repo" rev-parse HEAD)
    sha=$(git -C "$repo" show "$commit:shader-template.sealed" | shasum -a 256 | cut -d' ' -f1)
    printf '{\n  "commit": "%s",\n  "sha256": "%s"\n}\n' "$commit" "$sha" > "$ref"
    echo "$ref: $commit, $sha (commit it with the change that needs it)"
    exit 0
fi

tag=$1
out=build/release
mkdir -p "$out"
file="$out/shader-template.sealed"
git -C "$repo" show "$(field commit):shader-template.sealed" > "$file"
sha=$(shasum -a 256 "$file" | cut -d' ' -f1)
if [[ $sha != "$(field sha256)" ]]; then
    echo "the private repository's file at the pinned commit is not the pinned one ($sha)" >&2
    exit 1
fi
[[ $(head -c 8 "$file") == WSME0001 ]] || { echo "$file is not a sealed template" >&2; exit 1; }
if [[ $# == 2 ]]; then
    python3 tools/switch/shader_manifest.py open "$file" "$2" "$out/shader-template-check.bin"
    rm -f "$out/shader-template-check.bin"
fi
(cd "$out" && echo "$sha  shader-template.sealed" > shader-template.sealed.sha256)
gh release upload "$tag" "$file" "$file.sha256" --repo centollOS/SwitchWakerHD --clobber
echo "attached shader-template.sealed ($sha) to $tag"
