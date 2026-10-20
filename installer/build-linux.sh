#!/bin/bash
# build-linux.sh - Builds the Linux installer (Avalonia, .NET 8 self-contained, one file) from installer/desktop and
# installer/shared, one archive per architecture in installer/bin:
#   SwitchWakerHD-Installer-linux-x64.tar.gz, SwitchWakerHD-Installer-linux-arm64.tar.gz
# (a tar keeps the file executable; the player extracts it and double-clicks SwitchWakerHD-Installer).
#   installer/build-linux.sh [version]      (needs the .NET 8 SDK; builds on Linux or macOS)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
version=${1:-1.0.0}
version=${version#v}
out=$here/bin
work=$here/desktop/obj/linux
dotnet=${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}
mkdir -p "$out"
rm -rf "$work"

for rid in linux-x64 linux-arm64; do
    dir="$work/$rid/SwitchWakerHD-Installer"
    echo "== $rid -> $dir"
    # one file, its native libraries inside; no libicu needed (the language comes from LANG)
    "$dotnet" publish "$here/desktop/SwitchWakerHD.Installer.Desktop.csproj" -c Release -r "$rid" --self-contained \
        -p:Version="$version" -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true \
        -p:EnableCompressionInSingleFile=true -p:InvariantGlobalization=true -o "$dir" --nologo
    chmod +x "$dir/SwitchWakerHD-Installer"
    tgz="$out/SwitchWakerHD-Installer-$rid.tar.gz"
    # COPYFILE_DISABLE: on macOS, no ._ files or extended attributes in the archive
    COPYFILE_DISABLE=1 tar --no-xattrs -C "$work/$rid" -czf "$tgz" SwitchWakerHD-Installer
    (cd "$out" && shasum -a 256 "$(basename "$tgz")" > "$(basename "$tgz").sha256")
    echo "   $(du -h "$tgz" | cut -f1)  $tgz"
done
