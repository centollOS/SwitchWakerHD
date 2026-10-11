#Requires -Version 5.1
# SwitchWakerHD Easy Installer
# Source: github.com/centollOS/SwitchWakerHD

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# -----------------------------------------------------------------------
# Language detection  (es = Spanish, everything else = English)
# -----------------------------------------------------------------------
$lang = if ([System.Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName -eq 'es') { 'es' } else { 'en' }

# -----------------------------------------------------------------------
# String tables
# -----------------------------------------------------------------------
$S = @{
    es = @{
        banner1          = '     SwitchWakerHD  -  Instalador Automatico       '
        banner2          = '     Wind Waker HD para Nintendo Switch             '
        workdir          = 'Directorio de trabajo: {0}'
        sec_python       = 'Python 3'
        sec_devkit       = 'devkitPro Switch Toolchain'
        sec_release      = 'SwitchWakerHD {0}'
        sec_game         = 'Tu copia del juego'
        py_found         = 'Python encontrado: {0}'
        py_installing    = 'Instalando Python 3.12 via winget...'
        py_ok            = 'Python instalado correctamente.'
        py_fail          = "No se pudo instalar Python automaticamente.`nInstala Python 3 desde https://python.org y vuelve a ejecutar este instalador."
        py_pkgs          = 'Instalando dependencias Python (zstandard, pycryptodome)...'
        py_pkgs_ok       = 'Dependencias Python instaladas.'
        dkp_already      = 'devkitPro ya esta instalado.'
        dkp_searching    = 'Buscando dkp-pacman en C:\devkitPro...'
        dkp_found_at     = 'dkp-pacman encontrado en: {0}'
        dkp_downloading  = 'Descargando instalador de devkitPro v3.0.3...'
        dkp_verifying    = 'Verificando integridad del instalador...'
        dkp_verified     = 'Integridad verificada.'
        dkp_notice       = @(
            '',
            '  El instalador de devkitPro se va a abrir ahora.',
            '',
            '  Por favor, en el instalador:',
            '    1. Acepta el acuerdo de licencia',
            '    2. Deja la ruta por defecto: C:\devkitPro',
            '    3. Completa la instalacion y cierra el instalador',
            ''
        )
        dkp_press_open   = '  Presiona ENTER para abrir el instalador de devkitPro...'
        dkp_manual       = '  Si el instalador no ha terminado, completalo ahora.'
        dkp_press_done   = '  Presiona ENTER cuando hayas cerrado el instalador...'
        dkp_not_found    = "No se encontro dkp-pacman en: {0}`nAsegurate de que devkitPro quedo instalado en C:\devkitPro."
        dkp_installing   = 'Instalando paquetes Nintendo Switch...'
        dkp_pkg_warn     = 'Algunos paquetes pueden ya estar instalados. Continuando...'
        dkp_ready        = 'Toolchain Switch listo.'
        rel_checking     = 'Buscando la ultima version de SwitchWakerHD...'
        rel_found        = 'Version encontrada: {0}'
        rel_fallback     = 'No se pudo verificar la ultima version, usando {0}'
        rel_already      = 'Release {0} ya descargado.'
        rel_downloading  = 'Descargando SwitchWakerHD {0}...'
        rel_verifying    = 'Verificando integridad del release...'
        rel_verified     = 'Descarga verificada.'
        rel_extracting   = 'Extrayendo archivos...'
        rel_extracted    = 'Extraido en: {0}'
        rel_extracted_ok = 'Release ya extraido.'
        game_header      = @(
            '',
            '  Necesitas tu dump PERSONAL de Wind Waker HD (USA o EUR, version 0).',
            '  Formatos soportados:',
            ''
        )
        game_opt1        = '    [1] Imagen de disco  (.wux / .wud)  — necesita GAME.key y common.key'
        game_opt2        = '    [2] Archivo Cemu     (.wua)          — no necesita claves'
        game_opt3        = '    [3] Carpeta extraida (contiene code/, content/, meta/)'
        game_prompt      = '  Tu eleccion [1/2/3]'
        game_selected    = 'Juego seleccionado: {0}'
        game_invalid     = 'Opcion invalida. Vuelve a ejecutar el instalador.'
        game_none        = 'No se selecciono ningun archivo o carpeta valido.'
        dlg_title_img    = 'Selecciona la imagen del juego (.wux o .wud)'
        dlg_filter_img   = 'Imagen de disco (*.wux;*.wud)|*.wux;*.wud|Todos los archivos (*.*)|*.*'
        dlg_title_wua    = 'Selecciona el archivo Cemu (.wua)'
        dlg_filter_wua   = 'Archivo Cemu (*.wua)|*.wua|Todos los archivos (*.*)|*.*'
        dlg_folder       = 'Selecciona la carpeta raiz del juego (contiene code/, content/, meta/)'
        build_banner     = @(
            '  =================================================',
            '   Construyendo... puede tardar entre 10-30 min.  ',
            '   No cierres esta ventana.                        ',
            '  ================================================='
        )
        build_ok         = 'Build completado. Archivos en: {0}'
        build_fail       = 'El build fallo con codigo {0}. Revisa los mensajes de error arriba.'
        sd_header        = '  Copiar los archivos a tu tarjeta SD?'
        sd_hint          = '  Asegurate de que la SD este conectada a esta PC.'
        sd_prompt        = '  Letra de unidad de la SD (ej: E) o ENTER para saltar'
        sd_not_found     = 'No se encontro la unidad {0}. Copia manualmente desde:'
        sd_copying       = 'Copiando a {0}...'
        sd_copied        = 'Archivos copiados a {0}'
        sd_manual        = 'Copia manual requerida:'
        sd_src           = '  Origen : {0}'
        sd_dst           = '  Destino: raiz de tu tarjeta SD'
        final_title      = '    Instalacion completada exitosamente!           '
        next_header      = '  Proximos pasos en tu Nintendo Switch:'
        next_steps       = @(
            '   1. Inserta la SD con los archivos copiados',
            '   2. Abre el Homebrew Menu (manten R al lanzar cualquier juego)',
            '   3. Selecciona SwitchWakerHD',
            '   4. Primera vez: compilara graficos (~20 min), es normal'
        )
        press_close      = '  Presiona ENTER para cerrar...'
        err_not_admin    = "Este instalador debe ejecutarse como Administrador.`nHaz clic derecho en el .exe y elige 'Ejecutar como administrador'."
        err_download     = "No se pudo descargar: {0}`nVerifica tu conexion a internet."
        err_integrity    = "El archivo descargado no paso la verificacion SHA256.`nElimina la carpeta {1} e intenta de nuevo."
    }

    en = @{
        banner1          = '     SwitchWakerHD  -  Automatic Installer         '
        banner2          = '     Wind Waker HD for Nintendo Switch              '
        workdir          = 'Working directory: {0}'
        sec_python       = 'Python 3'
        sec_devkit       = 'devkitPro Switch Toolchain'
        sec_release      = 'SwitchWakerHD {0}'
        sec_game         = 'Your game dump'
        py_found         = 'Python found: {0}'
        py_installing    = 'Installing Python 3.12 via winget...'
        py_ok            = 'Python installed successfully.'
        py_fail          = "Could not install Python automatically.`nInstall Python 3 from https://python.org and run this installer again."
        py_pkgs          = 'Installing Python dependencies (zstandard, pycryptodome)...'
        py_pkgs_ok       = 'Python dependencies installed.'
        dkp_already      = 'devkitPro is already installed.'
        dkp_searching    = 'Searching for dkp-pacman in C:\devkitPro...'
        dkp_found_at     = 'dkp-pacman found at: {0}'
        dkp_downloading  = 'Downloading devkitPro installer v3.0.3...'
        dkp_verifying    = 'Verifying installer integrity...'
        dkp_verified     = 'Integrity verified.'
        dkp_notice       = @(
            '',
            '  The devkitPro installer is about to open.',
            '',
            '  In the installer, please:',
            '    1. Accept the license agreement',
            '    2. Keep the default install path: C:\devkitPro',
            '    3. Complete the installation and close the installer',
            ''
        )
        dkp_press_open   = '  Press ENTER to open the devkitPro installer...'
        dkp_manual       = '  If the installer has not finished, complete it now.'
        dkp_press_done   = '  Press ENTER once you have closed the installer...'
        dkp_not_found    = "dkp-pacman not found at: {0}`nMake sure devkitPro was installed to C:\devkitPro."
        dkp_installing   = 'Installing Nintendo Switch packages...'
        dkp_pkg_warn     = 'Some packages may already be installed. Continuing...'
        dkp_ready        = 'Switch toolchain ready.'
        rel_checking     = 'Checking for the latest SwitchWakerHD release...'
        rel_found        = 'Latest version: {0}'
        rel_fallback     = 'Could not check latest version, using {0}'
        rel_already      = 'Release {0} already downloaded.'
        rel_downloading  = 'Downloading SwitchWakerHD {0}...'
        rel_verifying    = 'Verifying release integrity...'
        rel_verified     = 'Download verified.'
        rel_extracting   = 'Extracting files...'
        rel_extracted    = 'Extracted to: {0}'
        rel_extracted_ok = 'Release already extracted.'
        game_header      = @(
            '',
            '  You need your PERSONAL dump of Wind Waker HD (USA or EUR, version 0).',
            '  Supported formats:',
            ''
        )
        game_opt1        = '    [1] Disc image       (.wux / .wud)  — needs GAME.key and common.key'
        game_opt2        = '    [2] Cemu archive     (.wua)          — no keys needed'
        game_opt3        = '    [3] Extracted folder (contains code/, content/, meta/)'
        game_prompt      = '  Your choice [1/2/3]'
        game_selected    = 'Game selected: {0}'
        game_invalid     = 'Invalid option. Please run the installer again.'
        game_none        = 'No valid file or folder was selected.'
        dlg_title_img    = 'Select the game disc image (.wux or .wud)'
        dlg_filter_img   = 'Disc image (*.wux;*.wud)|*.wux;*.wud|All files (*.*)|*.*'
        dlg_title_wua    = 'Select the Cemu archive (.wua)'
        dlg_filter_wua   = 'Cemu archive (*.wua)|*.wua|All files (*.*)|*.*'
        dlg_folder       = 'Select the game root folder (contains code/, content/, meta/)'
        build_banner     = @(
            '  =================================================',
            '   Building... this may take 10-30 minutes.        ',
            '   Do not close this window.                        ',
            '  ================================================='
        )
        build_ok         = 'Build complete. Output files at: {0}'
        build_fail       = 'Build failed with exit code {0}. Check the error messages above.'
        sd_header        = '  Copy the files to your SD card?'
        sd_hint          = '  Make sure your SD card is connected to this PC.'
        sd_prompt        = '  SD card drive letter (e.g. E) or ENTER to skip'
        sd_not_found     = 'Drive {0} not found. Copy manually from:'
        sd_copying       = 'Copying to {0}...'
        sd_copied        = 'Files copied to {0}'
        sd_manual        = 'Manual copy required:'
        sd_src           = '  Source : {0}'
        sd_dst           = '  Target : root of your SD card'
        final_title      = '    Installation completed successfully!            '
        next_header      = '  Next steps on your Nintendo Switch:'
        next_steps       = @(
            '   1. Insert the SD card with the copied files',
            '   2. Open Homebrew Menu (hold R while launching any game)',
            '   3. Select SwitchWakerHD',
            '   4. First launch: graphics will compile (~20 min), this is normal'
        )
        press_close      = '  Press ENTER to close...'
        err_not_admin    = "This installer must be run as Administrator.`nRight-click the .exe and choose 'Run as administrator'."
        err_download     = "Could not download: {0}`nCheck your internet connection."
        err_integrity    = "Downloaded file failed SHA256 verification.`nDelete the folder {1} and try again."
    }
}

function T([string]$key, [object[]]$fmtArgs) {
    $str = $S[$lang][$key]
    if ($null -eq $str) { return "[$key]" }
    if ($fmtArgs -and $fmtArgs.Count -gt 0) { return ($str -f $fmtArgs) }
    return $str
}

# -----------------------------------------------------------------------
# Console helpers
# -----------------------------------------------------------------------
function Write-Banner {
    Clear-Host
    Write-Host ''
    Write-Host '  =================================================' -ForegroundColor Blue
    Write-Host (T 'banner1') -ForegroundColor White
    Write-Host (T 'banner2') -ForegroundColor Gray
    Write-Host '  =================================================' -ForegroundColor Blue
    Write-Host ''
}

function Write-Step([string]$msg)  { Write-Host "  >> $msg" -ForegroundColor Cyan }
function Write-OK([string]$msg)    { Write-Host "  [OK] $msg" -ForegroundColor Green }
function Write-Warn([string]$msg)  { Write-Host "  [!!] $msg" -ForegroundColor Yellow }
function Write-Section([string]$n, [string]$title) {
    Write-Host ''
    Write-Host "  ---- [$n] $title ----" -ForegroundColor White
}

function Exit-WithError([string]$msg) {
    Write-Host ''
    Write-Host "  [ERROR] $msg" -ForegroundColor Red
    Write-Host ''
    Write-Host (T 'press_close') -ForegroundColor Gray
    $null = Read-Host
    exit 1
}

function Get-FileFromWeb([string]$url, [string]$dest) {
    try   { Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing }
    catch { Exit-WithError (T 'err_download' $url) }
}

function Confirm-SHA256([string]$file, [string]$expected) {
    $actual = (Get-FileHash -Path $file -Algorithm SHA256).Hash
    if ($actual -ne $expected.ToUpper()) {
        Remove-Item $file -Force
        Exit-WithError (T 'err_integrity' @($file, $WorkDir))
    }
}

# -----------------------------------------------------------------------
# Start
# -----------------------------------------------------------------------
Write-Banner

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]'Administrator')
if (-not $isAdmin) { Exit-WithError (T 'err_not_admin') }

# -----------------------------------------------------------------------
# Working directory
# -----------------------------------------------------------------------
$WorkDir = "$env:USERPROFILE\SwitchWakerHD-Build"
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Write-Step (T 'workdir' $WorkDir)

# -----------------------------------------------------------------------
# STEP 1 — Python 3
# -----------------------------------------------------------------------
Write-Section '1/5' (T 'sec_python')

$pythonCmd = $null
foreach ($candidate in @('python', 'python3', 'py')) {
    try {
        $ver = & $candidate --version 2>&1
        if ("$ver" -match 'Python 3\.') {
            $pythonCmd = $candidate
            Write-OK (T 'py_found' "$ver")
            break
        }
    }
    catch { }
}

if (-not $pythonCmd) {
    Write-Step (T 'py_installing')
    winget install --id Python.Python.3.12 --source winget `
        --accept-package-agreements --accept-source-agreements --silent
    if ($LASTEXITCODE -ne 0) { Exit-WithError (T 'py_fail') }
    $env:Path  = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                 [System.Environment]::GetEnvironmentVariable('Path','User')
    $pythonCmd = 'python'
    Write-OK (T 'py_ok')
}

Write-Step (T 'py_pkgs')
$ErrorActionPreference = 'Continue'
& $pythonCmd -m pip install --quiet zstandard pycryptodome
$ErrorActionPreference = 'Stop'
Write-OK (T 'py_pkgs_ok')

# -----------------------------------------------------------------------
# STEP 2 — devkitPro
# -----------------------------------------------------------------------
Write-Section '2/5' (T 'sec_devkit')

$DevkitRoot      = 'C:\devkitPro'
$DevkitInstaller = "$WorkDir\devkitProInstaller.exe"
$DevkitSHA256    = '038a99dc84f1ca0b52e9e0e074a94a3b0672e6d7bf0988563f0ab0812dcbb38d'

# Searches for dkp-pacman / pacman under the devkitPro root; returns path or $null.
function Find-DkpPacman([string]$root) {
    $candidates = @(
        "$root\devkitpro-pacman\bin\dkp-pacman.exe",
        "$root\devkitpro-pacman\usr\bin\pacman.exe",
        "$root\msys2\usr\bin\pacman.exe",
        "$root\pacman\usr\bin\pacman.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    # Recursive fallback — dkp-pacman first, then any pacman
    foreach ($name in @('dkp-pacman.exe', 'pacman.exe')) {
        $hit = Get-ChildItem $root -Recurse -Filter $name -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$DkpPacman = Find-DkpPacman $DevkitRoot

if (-not $DkpPacman) {
    Write-Step (T 'dkp_downloading')
    Get-FileFromWeb `
        'https://github.com/devkitPro/installer/releases/download/v3.0.3/devkitProUpdater-3.0.3.exe' `
        $DevkitInstaller

    Write-Step (T 'dkp_verifying')
    Confirm-SHA256 -file $DevkitInstaller -expected $DevkitSHA256
    Write-OK (T 'dkp_verified')

    foreach ($line in (T 'dkp_notice')) { Write-Host $line -ForegroundColor Yellow }
    Write-Host (T 'dkp_press_open') -ForegroundColor Cyan
    $null = Read-Host

    $ErrorActionPreference = 'Continue'
    Start-Process -FilePath $DevkitInstaller -Wait
    $ErrorActionPreference = 'Stop'

    $env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                [System.Environment]::GetEnvironmentVariable('Path','User')

    # Search again after install
    Write-Step (T 'dkp_searching')
    $DkpPacman = Find-DkpPacman $DevkitRoot

    if (-not $DkpPacman) {
        Write-Warn (T 'dkp_manual')
        Write-Host (T 'dkp_press_done') -ForegroundColor Cyan
        $null = Read-Host
        $env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                    [System.Environment]::GetEnvironmentVariable('Path','User')
        $DkpPacman = Find-DkpPacman $DevkitRoot
    }
}
else {
    Write-OK (T 'dkp_already')
}

if (-not $DkpPacman) { Exit-WithError (T 'dkp_not_found' $DevkitRoot) }
Write-OK (T 'dkp_found_at' $DkpPacman)

Write-Step (T 'dkp_installing')
$env:DEVKITPRO = $DevkitRoot
$ErrorActionPreference = 'Continue'
& $DkpPacman -Sy --noconfirm switch-dev deko3d uam switch-lz4 switch-zlib switch-cmake 2>&1
$dkpExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($dkpExit -ne 0) { Write-Warn (T 'dkp_pkg_warn') }

$env:DEVKITPRO = $DevkitRoot
$env:DEVKITARM = "$DevkitRoot\devkitARM"
$env:DEVKITPPC = "$DevkitRoot\devkitPPC"
$env:Path      = "$DevkitRoot\tools\bin;$DevkitRoot\devkitARM\bin;$env:Path"
Write-OK (T 'dkp_ready')

# -----------------------------------------------------------------------
# STEP 3 — Fetch latest SwitchWakerHD release
# -----------------------------------------------------------------------
$FallbackVer = 'v0.4.3'
Write-Section '3/5' (T 'sec_release' '...')

Write-Step (T 'rel_checking')
$ReleaseVer = $FallbackVer
try {
    $api = Invoke-WebRequest `
        -Uri 'https://api.github.com/repos/centollOS/SwitchWakerHD/releases/latest' `
        -UseBasicParsing
    $ReleaseVer = ($api.Content | ConvertFrom-Json).tag_name
    Write-OK (T 'rel_found' $ReleaseVer)
}
catch {
    Write-Warn (T 'rel_fallback' $FallbackVer)
}

$ReleaseDir = "$WorkDir\SwitchWakerHD-$ReleaseVer"
$ReleaseZip = "$WorkDir\SwitchWakerHD-$ReleaseVer.zip"

if (-not (Test-Path $ReleaseZip)) {
    Write-Step (T 'rel_downloading' $ReleaseVer)
    Get-FileFromWeb `
        "https://github.com/centollOS/SwitchWakerHD/releases/download/$ReleaseVer/SwitchWakerHD-$ReleaseVer.zip" `
        $ReleaseZip

    Write-Step (T 'rel_verifying')
    $sha256File = "$WorkDir\SwitchWakerHD-$ReleaseVer.zip.sha256"
    Get-FileFromWeb `
        "https://github.com/centollOS/SwitchWakerHD/releases/download/$ReleaseVer/SwitchWakerHD-$ReleaseVer.zip.sha256" `
        $sha256File
    $releaseHash = (Get-Content $sha256File -Raw).Split(' ')[0].Trim().ToUpper()
    Confirm-SHA256 -file $ReleaseZip -expected $releaseHash
    Write-OK (T 'rel_verified')
}
else {
    Write-OK (T 'rel_already' $ReleaseVer)
}

if (-not (Test-Path $ReleaseDir)) {
    Write-Step (T 'rel_extracting')
    Expand-Archive -Path $ReleaseZip -DestinationPath $ReleaseDir -Force
    Write-OK (T 'rel_extracted' $ReleaseDir)
}
else {
    Write-OK (T 'rel_extracted_ok')
}

# -----------------------------------------------------------------------
# STEP 4 — Game dump selection
# -----------------------------------------------------------------------
Write-Section '4/5' (T 'sec_game')

foreach ($line in (T 'game_header')) { Write-Host $line -ForegroundColor Gray }
Write-Host (T 'game_opt1') -ForegroundColor White
Write-Host (T 'game_opt2') -ForegroundColor White
Write-Host (T 'game_opt3') -ForegroundColor White
Write-Host ''
$choice = Read-Host (T 'game_prompt')

Add-Type -AssemblyName System.Windows.Forms

$gamePath = $null
$buildArg = $null

switch ($choice.Trim()) {
    '1' {
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title  = T 'dlg_title_img'
        $dlg.Filter = T 'dlg_filter_img'
        $null = $dlg.ShowDialog()
        $gamePath = $dlg.FileName
        $buildArg = '--image'
    }
    '2' {
        $dlg = New-Object System.Windows.Forms.OpenFileDialog
        $dlg.Title  = T 'dlg_title_wua'
        $dlg.Filter = T 'dlg_filter_wua'
        $null = $dlg.ShowDialog()
        $gamePath = $dlg.FileName
        $buildArg = '--wua'
    }
    '3' {
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description         = T 'dlg_folder'
        $dlg.ShowNewFolderButton = $false
        $null = $dlg.ShowDialog()
        $gamePath = $dlg.SelectedPath
        $buildArg = '--game-dir'
    }
    default { Exit-WithError (T 'game_invalid') }
}

if ([string]::IsNullOrWhiteSpace($gamePath) -or -not (Test-Path $gamePath)) {
    Exit-WithError (T 'game_none')
}
Write-OK (T 'game_selected' $gamePath)

# -----------------------------------------------------------------------
# STEP 5 — Build
# -----------------------------------------------------------------------
Write-Host ''
foreach ($line in (T 'build_banner')) {
    $color = if ($line -match '=+') { 'Blue' } elseif ($line -match 'cerrar|close') { 'Yellow' } else { 'White' }
    Write-Host $line -ForegroundColor $color
}
Write-Host ''

Set-Location $ReleaseDir
$ErrorActionPreference = 'Continue'
& $pythonCmd 'tools/switch/make_sd.py' $buildArg $gamePath
$buildExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'

if ($buildExit -ne 0) { Exit-WithError (T 'build_fail' $buildExit) }

$sdOutputDir = "$ReleaseDir\build\sd"
Write-OK (T 'build_ok' $sdOutputDir)

# -----------------------------------------------------------------------
# SD copy (optional)
# -----------------------------------------------------------------------
Write-Host ''
Write-Host (T 'sd_header') -ForegroundColor White
Write-Host (T 'sd_hint')   -ForegroundColor Gray
Write-Host ''
$sdLetter = Read-Host (T 'sd_prompt')

if ($sdLetter -match '^[A-Za-z]$') {
    $sdDrive = "$($sdLetter.ToUpper()):"
    if (-not (Test-Path "$sdDrive\")) {
        Write-Warn (T 'sd_not_found' $sdDrive)
        Write-Host "  $sdOutputDir" -ForegroundColor Gray
    }
    else {
        Write-Step (T 'sd_copying' $sdDrive)
        $ErrorActionPreference = 'Continue'
        robocopy "$sdOutputDir\" "$sdDrive\" /E /NFL /NDL /NJH /NJS /NC /NS
        $ErrorActionPreference = 'Stop'
        Write-OK (T 'sd_copied' $sdDrive)
    }
}
else {
    Write-Host ''
    Write-Warn (T 'sd_manual')
    Write-Host (T 'sd_src' $sdOutputDir) -ForegroundColor Gray
    Write-Host (T 'sd_dst')              -ForegroundColor Gray
}

# -----------------------------------------------------------------------
# Done
# -----------------------------------------------------------------------
Write-Host ''
Write-Host '  =================================================' -ForegroundColor Green
Write-Host (T 'final_title')                                      -ForegroundColor Green
Write-Host '  =================================================' -ForegroundColor Green
Write-Host ''
Write-Host (T 'next_header') -ForegroundColor White
foreach ($line in (T 'next_steps')) { Write-Host $line -ForegroundColor Gray }
Write-Host ''
Write-Host (T 'press_close') -ForegroundColor Gray
$null = Read-Host
