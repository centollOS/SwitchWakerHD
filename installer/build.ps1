#Requires -Version 5.1
# build.ps1 - Builds SwitchWakerHD-Installer.exe (WPF, .NET Framework 4.8) from installer/src.
# Run this once on Windows to generate the distributable installer.

$ErrorActionPreference = 'Stop'
$OutputDir  = Join-Path $PSScriptRoot "bin"

Write-Host ""
Write-Host "  SwitchWakerHD - Installer builder (.NET Framework 4.8)" -ForegroundColor Cyan
Write-Host ""

# Install any .NET SDK if missing (net48 compiles with any modern SDK)
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Write-Host "  Installing .NET SDK via winget..." -ForegroundColor White
    winget install Microsoft.DotNet.SDK.8 --accept-package-agreements --accept-source-agreements --silent
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("Path","User")
}

$sdkVer = dotnet --version 2>$null
Write-Host "  [OK] .NET SDK: $sdkVer" -ForegroundColor Green

# Build
$projects = @(
    @{ Proj = "$PSScriptRoot\src\SwitchWakerHD.Installer.csproj"; Exe = "SwitchWakerHD-Installer.exe" }
)
foreach ($p in $projects) {
    Write-Host ""
    Write-Host "  Building $($p.Exe) (net48)..." -ForegroundColor White
    Write-Host ""
    dotnet publish $p.Proj --configuration Release --output "$OutputDir" --nologo
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "  [ERROR] Build of $($p.Exe) failed. Check the output above." -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "  =================================================" -ForegroundColor Green
foreach ($p in $projects) {
    $exe = Get-Item "$OutputDir\$($p.Exe)"
    Write-Host ("  [OK] {0}  ({1} KB)" -f $p.Exe, [math]::Round($exe.Length / 1KB, 0)) -ForegroundColor Green
}
Write-Host "  =================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Path: $OutputDir" -ForegroundColor Gray
Write-Host ""
Write-Host "  NOTE: Windows SmartScreen will warn 'unknown app' on first run" -ForegroundColor Yellow
Write-Host "  because the exe is not code-signed. Click More info > Run anyway." -ForegroundColor Yellow
Write-Host ""
