# Builds dist\GogglesCast\GogglesCast.exe with the capture program (native\) next to it.
# FFmpeg is not shipped: the stream needs only native\gcast_capture.exe.
# -Installer also builds installer\GogglesCast-Setup-<version>.exe with Inno Setup 6 (winget install JRSoftware.InnoSetup).
param([switch]$Installer, [string]$Version = "1.0.0")
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
python -m PyInstaller --noconfirm --onedir --windowed --name GogglesCast --distpath dist --workpath build GogglesCast.pyw
# Apollo's capture + encoder path (native\build.sh, MSYS2)
if (-not (Test-Path native\dist\gcast_capture.exe)) { & C:\msys64\usr\bin\bash.exe -lc /c/code/goggles-cast/native/build.sh }
if (-not (Test-Path native\dist\gcast_capture.exe)) { throw 'native\dist\gcast_capture.exe missing: run native/build.sh in MSYS2' }
Copy-Item native\dist dist\GogglesCast\native -Recurse -Force
Remove-Item -Recurse -Force dist\GogglesCast\vdisplay, dist\GogglesCast\edition-direct -ErrorAction SilentlyContinue
Write-Host "OK: dist\GogglesCast\GogglesCast.exe"
if ($Installer) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 not found: winget install JRSoftware.InnoSetup' }
    $env:GC_VERSION = $Version
    & $iscc /Qp installer.iss
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
    Get-ChildItem installer\*.exe | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,1)}}
}
