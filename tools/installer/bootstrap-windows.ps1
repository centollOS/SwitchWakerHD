# Wind Waker HD setup for Windows (started by "Install Wind Waker HD.bat").
# Gets the pinned embeddable Python from python.org (SHA-256 checked, no installation, no admin
# rights) into %LOCALAPPDATA%\WWHD\python, then runs tools\installer\setup.py with it.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with its progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$pkg = Resolve-Path (Join-Path $PSScriptRoot '..\..')
# downloaded files carry the "mark of the web"; the user started this setup, so allow its helpers
Get-ChildItem -Path $pkg -Recurse -File | Unblock-File -ErrorAction SilentlyContinue

$pins = Get-Content -Raw (Join-Path $PSScriptRoot 'toolchains.json') | ConvertFrom-Json
$py = $pins.python.windows
$data = if ($env:WWHD_DATA_DIR) { $env:WWHD_DATA_DIR } else { Join-Path $env:LOCALAPPDATA 'WWHD' }
$pydir = Join-Path $data (Join-Path 'python' $py.dir)
$pyexe = Join-Path $pydir 'python.exe'
try {
    if (-not (Test-Path $pyexe)) {
        Write-Host "Getting Python for the setup (about 11 MB, once)..."
        New-Item -ItemType Directory -Force -Path $pydir | Out-Null
        $zip = Join-Path $data 'python-embed.zip'
        Invoke-WebRequest -UseBasicParsing -Uri $py.url -OutFile $zip
        $sum = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLower()
        if ($sum -ne $py.sha256) {
            Remove-Item $zip -Force
            throw "The Python download is corrupt or was changed (SHA-256 mismatch). Nothing was installed."
        }
        Expand-Archive -Path $zip -DestinationPath $pydir -Force
        Remove-Item $zip -Force
    }
} catch {
    Write-Host ""
    Write-Host "Setup could not get Python: $($_.Exception.Message)"
    Write-Host "Check your internet connection and run the setup again."
    exit 1
}
& $pyexe (Join-Path $pkg 'tools\installer\setup.py') @args
exit $LASTEXITCODE
