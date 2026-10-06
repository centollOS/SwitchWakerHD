#!/bin/sh
# Wind Waker HD setup in a terminal (Linux): the fallback for the wind-waker-hd program next to tools/.
cd "$(dirname "$0")/.." || exit 1
DIR="$(pwd)"
export PYTHONDONTWRITEBYTECODE=1
for p in python3 python; do
  if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
    exec "$p" "$DIR/tools/installer/setup.py" "$@"
  fi
done
# No Python 3.8+: fetch the pinned standalone build (tools/installer/toolchains.json, SHA-256 checked),
# into the release folder (portable release) or the per-user data folder
if [ -f "$DIR/portable.txt" ]; then DATA="$DIR/data"; else DATA="${XDG_DATA_HOME:-$HOME/.local/share}/wwhd"; fi
PYDIR="$DATA/python"
if [ ! -x "$PYDIR/bin/python3" ]; then
  case "$(uname -m)" in aarch64|arm64) KEY=linux-aarch64 ;; *) KEY=linux ;; esac
  URL=$(sed -n "/\"$KEY\": {/,/}/s/.*\"url\": \"\([^\"]*\)\".*/\1/p" "$DIR/tools/installer/toolchains.json" | tail -1)
  SUM=$(sed -n "/\"$KEY\": {/,/}/s/.*\"sha256\": \"\([^\"]*\)\".*/\1/p" "$DIR/tools/installer/toolchains.json" | tail -1)
  echo "Python 3 was not found; downloading a private copy for the setup (about 75 MB)."
  mkdir -p "$DATA" || exit 1
  TMP="$DATA/python.tar.gz"
  if command -v curl >/dev/null 2>&1; then curl -fL -o "$TMP" "$URL" || exit 1
  else wget -O "$TMP" "$URL" || exit 1; fi
  echo "$SUM  $TMP" | sha256sum -c - >/dev/null 2>&1 || { echo "The Python download is corrupt (SHA-256 mismatch)."; rm -f "$TMP"; exit 1; }
  rm -rf "$PYDIR" && tar -xzf "$TMP" -C "$DATA" && rm -f "$TMP" || exit 1
fi
exec "$PYDIR/bin/python3" "$DIR/tools/installer/setup.py" "$@"
