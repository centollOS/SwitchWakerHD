#Requires -Version 5.1
# build.ps1 — Generates SwitchWakerHD-Installer.exe from install.ps1 using ps2exe.
# Run this script once on Windows PowerShell to produce the distributable installer.

$ErrorActionPreference = 'Stop'

Write-Host ''
Write-Host '  SwitchWakerHD — Installer EXE builder' -ForegroundColor Cyan
Write-Host ''

if (-not (Get-Module -ListAvailable -Name ps2exe)) {
    Write-Host '  Installing ps2exe module...' -ForegroundColor White
    Install-Module -Name ps2exe -Scope CurrentUser -Force -AllowClobber
    Write-Host '  [OK] ps2exe installed.' -ForegroundColor Green
}
else {
    Write-Host '  [OK] ps2exe already available.' -ForegroundColor Green
}

Import-Module ps2exe

$inputFile  = Join-Path $PSScriptRoot 'install.ps1'
$outputFile = Join-Path $PSScriptRoot 'SwitchWakerHD-Installer.exe'

if (-not (Test-Path $inputFile)) {
    Write-Host "  [ERROR] install.ps1 not found at: $PSScriptRoot" -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host '  Compiling install.ps1 -> SwitchWakerHD-Installer.exe ...' -ForegroundColor White
Write-Host ''

Invoke-ps2exe `
    -inputFile  $inputFile `
    -outputFile $outputFile `
    -title       'SwitchWakerHD Installer' `
    -description 'Automatic installer for SwitchWakerHD - Wind Waker HD on Nintendo Switch' `
    -company     'centollOS' `
    -product     'SwitchWakerHD' `
    -version     '1.0.0.0' `
    -copyright   'SwitchWakerHD by centollOS (GPLv3) - github.com/centollOS/SwitchWakerHD' `
    -requireAdmin `
    -sta

if (-not (Test-Path $outputFile)) {
    Write-Host ''
    Write-Host '  [ERROR] EXE was not generated. Review ps2exe output above.' -ForegroundColor Red
    exit 1
}

$size = [math]::Round((Get-Item $outputFile).Length / 1KB, 1)
Write-Host ''
Write-Host "  [OK] SwitchWakerHD-Installer.exe generated ($size KB)" -ForegroundColor Green
Write-Host "  Path: $outputFile" -ForegroundColor Gray
Write-Host ''
Write-Host '  NOTE: Windows SmartScreen will warn about an unknown app on first run' -ForegroundColor Yellow
Write-Host '  because the exe is not code-signed. Users click More info > Run anyway.' -ForegroundColor Yellow
Write-Host ''
