#!/usr/bin/env bash
# build-linux.sh - Builds the Linux installer: one self-contained file (.NET 8 and Avalonia inside),
# installer/bin-linux/SwitchWakerHD-Installer-linux-x64. Needs the .NET 8 SDK
# (https://learn.microsoft.com/dotnet/core/install/linux, or ./dotnet-install.sh --channel 8.0).
set -euo pipefail
cd "$(dirname "$0")"
dotnet=$(command -v dotnet || echo "$HOME/.dotnet/dotnet")
[[ -x $dotnet ]] || { echo "the .NET 8 SDK is needed: https://learn.microsoft.com/dotnet/core/install/linux" >&2; exit 1; }
out=bin-linux
"$dotnet" publish linux/SwitchWakerHD.Installer.Linux.csproj --configuration Release --output "$out" --nologo
mv -f "$out/SwitchWakerHD-Installer" "$out/SwitchWakerHD-Installer-linux-x64"
chmod +x "$out/SwitchWakerHD-Installer-linux-x64"
echo "built $out/SwitchWakerHD-Installer-linux-x64 ($(du -h "$out/SwitchWakerHD-Installer-linux-x64" | cut -f1))"
