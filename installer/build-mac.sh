#!/bin/bash
# build-mac.sh - Builds the macOS installer: "SwitchWakerHD Installer.app" (Avalonia, .NET 8 self-contained) from
# installer/desktop and installer/shared, one zip per architecture in installer/bin:
#   SwitchWakerHD-Installer-macOS-AppleSilicon.zip (osx-arm64), SwitchWakerHD-Installer-macOS-Intel.zip (osx-x64)
# The app is signed ad hoc (no Apple Developer ID): the first time, macOS asks to allow it in
# System Settings > Privacy & Security > Open Anyway (INSTALL.md).
#   installer/build-mac.sh [version]      (needs the .NET 8 SDK; sips and iconutil come with macOS)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
version=${1:-1.0.0}
version=${version#v}
out=$here/bin
work=$here/desktop/obj/app
dotnet=${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}
mkdir -p "$out"
rm -rf "$work"

# AppIcon.icns from the crab logo
iconset=$work/AppIcon.iconset
mkdir -p "$iconset"
for s in 16 32 128 256 512; do
    sips -z $s $s "$here/windows/Assets/logo.png" --out "$iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$here/windows/Assets/logo.png" --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$work/AppIcon.icns"

for pair in osx-arm64:AppleSilicon osx-x64:Intel; do
    rid=${pair%%:*}
    name=${pair##*:}
    app="$work/$name/SwitchWakerHD Installer.app"
    echo "== $rid -> $app"
    "$dotnet" publish "$here/desktop/SwitchWakerHD.Installer.Desktop.csproj" -c Release -r "$rid" --self-contained \
        -p:Version="$version" -o "$app/Contents/MacOS" --nologo
    mkdir -p "$app/Contents/Resources"
    cp "$work/AppIcon.icns" "$app/Contents/Resources/"
    sed "s/@VERSION@/$version/g" "$here/desktop/macos/Info.plist" > "$app/Contents/Info.plist"
    codesign --force --deep --sign - "$app"
    codesign --verify --deep --strict "$app"
    zip="$out/SwitchWakerHD-Installer-macOS-$name.zip"
    rm -f "$zip"
    ditto -c -k --keepParent "$app" "$zip"
    (cd "$out" && shasum -a 256 "$(basename "$zip")" > "$(basename "$zip").sha256")
    echo "   $(du -h "$zip" | cut -f1)  $zip"
done
