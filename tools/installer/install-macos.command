#!/bin/bash
# Wind Waker HD setup in Terminal (macOS): the fallback for the "Wind Waker HD" app next to tools/.
# Double-click it (the first time: right-click > Open), or run it in Terminal.
cd "$(dirname "$0")/.." || exit 1
DIR="$(pwd)"
export PYTHONDONTWRITEBYTECODE=1
# This release folder was downloaded, so macOS marks every file in it as quarantined. You opened this
# script (or the app), so its helper tools are allowed to run too.
/usr/bin/xattr -dr com.apple.quarantine "$DIR" 2>/dev/null
clt_ok() { xcode-select -p >/dev/null 2>&1 && xcrun --find clang >/dev/null 2>&1; }
if ! clt_ok; then
  echo "Wind Waker HD setup needs Apple's free Command Line Tools (compiler and Python)."
  echo "A system dialog opens now: click \"Install\" and accept the license (a few minutes)."
  xcode-select --install >/dev/null 2>&1
  until clt_ok; do sleep 5; done
  echo "Command Line Tools installed."
fi
exec /usr/bin/python3 "$DIR/tools/installer/setup.py" "$@"
