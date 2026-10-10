#!/usr/bin/env bash
# Build the HOME-menu forwarder: an NSP that installs a "Wind Waker HD" icon on the Switch HOME
# screen and starts sdmc:/switch/wwhd/wwhd.nro as an application (title mode: all the memory, and
# apm accepts the GPU profile), without hbmenu. Ported from centollOS (scripts/switch there).
#
#   tools/switch/forwarder/build_forwarder.sh [--keys PATH] [--icon TGA] [--no-verify]
#
#   --keys PATH   the console's own keys (default ~/.switch/prod.keys). They are only bind-mounted
#                 read-only into the container for the pack and verify steps; nothing copies them
#                 and this script checks that no key value reached its logs.
#   --icon TGA    the icon's source (default: meta/iconTex.tga of the first dump in Rom/Decrypted)
#   --no-verify   skip the offline hactool check of the NSP
#
# Output: build/forwarder/wwhd_forwarder.nsp (INSTALL.md: installing it).
#
# How it works (built from source in a devkitPro container):
#   exefs    nx-hbloader v2.4.5 (switchbrew, pinned in forwarder.env) with nx-hbloader-forwarder.patch: it
#            loads the NRO named by romfs:/nextNroPath with the argv of romfs:/nextArgv (the SAK /
#            nsp-forwarder convention), and goes back to the HOME menu when the NRO returns instead
#            of starting hbmenu. main.npdm is hbloader's hbl.json with our title ID and
#            application_type 1 (application): pool 0, the application's memory.
#   romfs    nextNroPath and nextArgv = sdmc:/switch/wwhd/wwhd.nro (argv[0] is the NRO's path, as
#            hbmenu passes it).
#   control  control.nacp from make_nacp.py and the game's own icon (make_icon.py: iconTex.tga of
#            your dump scaled to 256x256; it stays in build/) in every language slot.
#   pack     hacBrewPack v3.05 (pinned in forwarder.env), no logo section.
#
# Title ID 0x01FF575748440000 (TITLE_ID in forwarder.env, which also holds the pins): "01FF" is outside
# the ranges used by retail titles (0100...) and "57574844" is ASCII "WWHD"; the low 12 bits are 0 as for
# any base application. Override with WWHD_FORWARDER_TITLE_ID if it ever collides.
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
here=$root/tools/switch/forwarder
image=${WWHD_FORWARDER_IMAGE:-localhost/wwrecomp-switch-native-build:2026-10-03}  # devkitA64 + host tools (centollOS Containerfile.native)

container_engine() { command -v podman >/dev/null && echo podman || { command -v docker >/dev/null && echo docker; } || true; }
container_run() {  # container_run ENGINE ROOT [run options...] IMAGE COMMAND...
    local engine=$1 root=$2
    shift 2
    if [[ $engine == podman ]]; then
        podman run --rm --userns=keep-id -v "$root:/work:Z" -w /work "$@"
    else
        docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/work" -w /work "$@"
    fi
}

# TITLE_ID and the pinned sources are in forwarder.env, shared with build_forwarder_windows.py
# (tr: a Windows checkout may have turned the file into CRLF).
eval "$(tr -d '\r' <"$here/forwarder.env")"
title_id=${WWHD_FORWARDER_TITLE_ID:-$TITLE_ID}
title_id=$(printf '%s' "${title_id#0x}" | tr 'A-F' 'a-f')
nro_path=sdmc:/switch/wwhd/wwhd.nro
name="Wind Waker HD"
publisher="SwitchWakerHD"
hbloader_url=$HBLOADER_URL hbloader_rev=$HBLOADER_COMMIT
hacbrewpack_url=$HACBREWPACK_URL hacbrewpack_rev=$HACBREWPACK_COMMIT

keys=$HOME/.switch/prod.keys verify=1 icon_src=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --keys) keys=$2; shift 2 ;;
        --icon) icon_src=$2; shift 2 ;;
        --no-verify) verify=0; shift ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "build_forwarder: unknown option $1" >&2; exit 2 ;;
    esac
done
if [[ ! -r $keys ]]; then
    echo "build_forwarder: cannot read the keys at $keys (--keys)" >&2
    exit 1
fi
keys=$(cd "$(dirname "$keys")" && pwd)/$(basename "$keys")
case $keys in
    "$root"/*) echo "build_forwarder: keep the keys outside the checkout ($keys)" >&2; exit 1 ;;
esac

engine=$(container_engine)
if [[ -z $engine ]]; then
    echo "build_forwarder: podman or docker is required" >&2
    exit 1
fi
if [[ -z $icon_src ]]; then
    icon_src=$(ls "$root"/Rom/Decrypted/*/meta/iconTex.tga 2>/dev/null | head -1 || true)
fi
if [[ ! -f $icon_src ]]; then
    echo "build_forwarder: no icon: give --icon path/to/meta/iconTex.tga (from your dump)" >&2
    exit 1
fi

out=$root/build/forwarder
mkdir -p "$out/src"
fetch() {  # fetch DIR URL REV
    local dir=$out/src/$1
    if [[ ! -d $dir/.git ]]; then
        git clone --quiet "$2" "$dir"
    fi
    if ! git -C "$dir" cat-file -e "$3^{commit}" 2>/dev/null; then
        git -C "$dir" fetch --quiet origin
    fi
    git -C "$dir" checkout --quiet --force "$3"
    git -C "$dir" clean --quiet -fdx
}
fetch nx-hbloader "$hbloader_url" "$hbloader_rev"
fetch hacBrewPack "$hacbrewpack_url" "$hacbrewpack_rev"
git -C "$out/src/nx-hbloader" apply "$here/nx-hbloader-forwarder.patch"
icon=$out/icon.jpg
if python3 -c "import PIL" 2>/dev/null; then
    python3 "$here/make_icon.py" "$icon_src" "$icon"
else
    uv run --quiet --with pillow python3 "$here/make_icon.py" "$icon_src" "$icon"
fi

# The display version (NACP, at most 15 bytes): git describe without the "v", without the hash
# when that is too long.
describe=$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo 0)
version=${describe#v}
if [[ ${#version} -gt 15 ]]; then
    version=${version%-g*}
    version=${version:0:15}
fi

# 1. Build hbloader (exefs) and hacBrewPack, without the keys.
container_run "$engine" "$root" -e TITLE_ID="$title_id" "$image" bash -lc '
set -euo pipefail
export DEVKITPRO=/opt/devkitpro PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
cd /work/build/forwarder/src/nx-hbloader
python3 /work/tools/switch/forwarder/patch_hbl.py hbl.json "$TITLE_ID"
make -s RELEASE=1 >/dev/null
cd /work/build/forwarder/src/hacBrewPack
cp config.mk.template config.mk
make -s >/dev/null 2>&1 || make
'

# 2. Lay out the NSP's inputs.
pack=$out/pack
rm -rf "$pack"
mkdir -p "$pack/exefs" "$pack/romfs" "$pack/control"
cp "$out/src/nx-hbloader/hbl.nso" "$pack/exefs/main"
cp "$out/src/nx-hbloader/hbl.npdm" "$pack/exefs/main.npdm"
printf '%s' "$nro_path" >"$pack/romfs/nextNroPath"
printf '%s' "$nro_path" >"$pack/romfs/nextArgv"
python3 "$here/make_nacp.py" "$pack/control/control.nacp" --title-id "$title_id" \
    --name "$name" --publisher "$publisher" --version "$version"
for lang in AmericanEnglish BritishEnglish Japanese French German LatinAmericanSpanish Spanish \
            Italian Dutch CanadianFrench Portuguese Russian Korean TraditionalChinese \
            SimplifiedChinese BrazilianPortuguese; do
    cp "$icon" "$pack/control/icon_$lang.dat"
done

# 3. Pack (and verify) with the keys mounted read-only for this run only. The log stays in
# memory and is checked for key values before it is printed.
log=$(container_run "$engine" "$root" -v "$keys:/keys/prod.keys:ro" -e TITLE_ID="$title_id" \
    -e VERIFY="$verify" "$image" bash -lc '
set -euo pipefail
export PATH=/opt/devkitpro/tools/bin:$PATH
cd /work/build/forwarder/pack
/work/build/forwarder/src/hacBrewPack/hacbrewpack -k /keys/prod.keys --titleid "$TITLE_ID" --nologo \
    --exefsdir exefs --romfsdir romfs --controldir control --nspdir nsp --ncadir nca \
    --tempdir temp --backupdir backup 2>&1 | grep -v -i "key" || true
nsp=nsp/$TITLE_ID.nsp
test -s "$nsp"
[[ $VERIFY == 1 ]] || exit 0
echo "== verify: hactool, offline"
rm -rf verify && mkdir -p verify/pfs
hactool -k /keys/prod.keys -t pfs0 --outdir=verify/pfs "$nsp" 2>&1 | grep -v -i "key" || true
for nca in verify/pfs/*.nca; do
    base=$(basename "$nca" .nca)
    info=$(hactool -k /keys/prod.keys "$nca" 2>&1)
    type=$(printf "%s\n" "$info" | sed -n "s/^Content Type: *//p" | head -1)
    tid=$(printf "%s\n" "$info" | sed -n "s/^Title ID: *//p" | head -1)
    echo "nca $base.nca type=$type title_id=$tid $(stat -c %s "$nca") bytes"
    case $type in
        Program) hactool -k /keys/prod.keys --exefsdir=verify/exefs --romfsdir=verify/romfs "$nca" >/dev/null 2>&1 ;;
        Control) hactool -k /keys/prod.keys --romfsdir=verify/control "$nca" >/dev/null 2>&1 ;;
        Meta) hactool -k /keys/prod.keys --section0dir=verify/meta "$nca" >/dev/null 2>&1 ;;
    esac
done
'
)
# Refuse to show (or keep) a log that contains any key value.
if grep -F -q -i -f <(grep -E -o '=[[:space:]]*[0-9A-Fa-f]{16,}' "$keys" | tr -d '= \t') <<<"$log"; then
    echo "build_forwarder: the container log contained key material; not printing it" >&2
    exit 1
fi
printf '%s\n' "$log"

nsp=$out/wwhd_forwarder.nsp
cp "$pack/nsp/$title_id.nsp" "$nsp"

if [[ $verify == 1 ]]; then
    python3 - "$pack/verify" "$title_id" "$name" "$publisher" "$version" "$nro_path" \
        "$icon" <<'EOF'
import glob, os, struct, sys
d, tid, name, pub, ver, nro, icon = sys.argv[1:]
tid = int(tid, 16)
fail = []
def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fail.append(what)
nacp = open(f"{d}/control/control.nacp", "rb").read()
cstr = lambda b: b.split(b"\0", 1)[0].decode()
check(cstr(nacp[0:0x200]) == name, f"NACP title {cstr(nacp[0:0x200])!r}")
check(cstr(nacp[0x200:0x300]) == pub, f"NACP publisher {cstr(nacp[0x200:0x300])!r}")
check(cstr(nacp[0x3060:0x3070]) == ver, f"NACP version {cstr(nacp[0x3060:0x3070])!r}")
check(nacp[0x3025] == 0, "NACP StartupUserAccount = None")
check(nacp[0x3035] == 1, "NACP VideoCapture = Manual (capture allowed, no 96 MiB recording reserve)")
check(struct.unpack_from("<Q", nacp, 0x3078)[0] == tid, "NACP SaveDataOwnerId = title ID")
ref = open(icon, "rb").read()
for lang in ("AmericanEnglish", "Spanish", "LatinAmericanSpanish"):
    p = f"{d}/control/icon_{lang}.dat"
    check(os.path.exists(p) and open(p, "rb").read() == ref, f"icon_{lang}.dat is the icon")
npdm = open(f"{d}/exefs/main.npdm", "rb").read()
check(npdm[:4] == b"META", "main.npdm magic META")
aci_off = struct.unpack_from("<I", npdm, 0x70)[0]
acid_off = struct.unpack_from("<I", npdm, 0x78)[0]
check(struct.unpack_from("<Q", npdm, aci_off + 0x10)[0] == tid, f"main.npdm ACI0 program ID 0x{struct.unpack_from('<Q', npdm, aci_off + 0x10)[0]:016x}")
lo, hi = struct.unpack_from("<QQ", npdm, acid_off + 0x210)
check(lo <= tid <= hi, f"main.npdm ACID program ID range 0x{lo:016x}-0x{hi:016x}")
check(npdm[0xC] & 1 == 1, "main.npdm 64-bit")
check(os.path.getsize(f"{d}/exefs/main") > 0 and open(f"{d}/exefs/main", "rb").read(4) == b"NSO0", "exefs/main is an NSO")
# Kernel capability "application type": low 14 bits 0b01111111111111 (0x1FFF), the value in
# bits 14..16; 1 = application.
kc_off, kc_size = struct.unpack_from("<II", npdm, aci_off + 0x30)
apptype = None
for i in range(0, kc_size, 4):
    v = struct.unpack_from("<I", npdm, aci_off + kc_off + i)[0]
    if v & 0x3FFF == 0x1FFF:
        apptype = (v >> 14) & 7
check(apptype == 1, f"main.npdm kernel capability application_type = {apptype}")
for f in ("nextNroPath", "nextArgv"):
    v = open(f"{d}/romfs/{f}").read()
    check(v == nro, f"romfs {f} = {v!r}")
sys.exit(1 if fail else 0)
EOF
fi

shasum -a 256 "$nsp" 2>/dev/null || sha256sum "$nsp"
printf 'Built %s (%s bytes): title ID 0x%s, "%s" %s -> %s\n' "$nsp" \
    "$(wc -c <"$nsp" | tr -d ' ')" "$title_id" "$name" "$version" "$nro_path"
