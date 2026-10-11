#Requires -Version 5.1
# build.ps1 — Builds SwitchWakerHD-Installer.exe from the C# project.
# Run this once on Windows to generate the distributable installer.

$ErrorActionPreference = 'Stop'
$ProjectDir = Join-Path $PSScriptRoot "src"
$OutputDir  = Join-Path $PSScriptRoot "bin"

Write-Host ""
Write-Host "  SwitchWakerHD — Installer builder (.NET 8)" -ForegroundColor Cyan
Write-Host ""

# ── Install .NET 8 SDK if missing ─────────────────────────────────────────────
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Write-Host "  Installing .NET 8 SDK via winget..." -ForegroundColor White
    winget install Microsoft.DotNet.SDK.8 --accept-package-agreements --accept-source-agreements --silent
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("Path","User")
}

$sdkVer = dotnet --version 2>$null
Write-Host "  [OK] .NET SDK: $sdkVer" -ForegroundColor Green

# ── Build ─────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  Publishing SwitchWakerHD-Installer.exe (self-contained, win-x64)..." -ForegroundColor White
Write-Host ""

dotnet publish "$ProjectDir\SwitchWakerHD.Installer.csproj" `
    --configuration Release `
    --runtime win-x64 `
    --self-contained true `
    -p:PublishSingleFile=true `
    -p:EnableCompressionInSingleFile=true `
    --output "$OutputDir" `
    --nologo

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  [ERROR] Build failed. Check the output above." -ForegroundColor Red
    exit 1
}

$exe  = Get-Item "$OutputDir\SwitchWakerHD-Installer.exe"
$size = [math]::Round($exe.Length / 1MB, 1)

Write-Host ""
Write-Host "  =================================================" -ForegroundColor Green
Write-Host "  [OK] SwitchWakerHD-Installer.exe  ($size MB)    " -ForegroundColor Green
Write-Host "  =================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Path: $($exe.FullName)" -ForegroundColor Gray
Write-Host ""
Write-Host "  NOTE: Windows SmartScreen will warn 'unknown app' on first run" -ForegroundColor Yellow
Write-Host "  because the exe is not code-signed. Click More info > Run anyway." -ForegroundColor Yellow
Write-Host ""
