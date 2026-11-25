# XFlySim-Client Windows build script (PyInstaller)
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI
# (GBK on zh-CN) when there is no UTF-8 BOM, which corrupts non-ASCII text.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build_exe.ps1
#         powershell -ExecutionPolicy Bypass -File build_exe.ps1 -OneDir
param(
    [switch]$OneDir
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

Write-Host "[1/3] Building webui/dist ..."
Push-Location "$root\webui"
if (-not (Test-Path "node_modules")) { npm ci }
npm run build
if ($LASTEXITCODE -ne 0) { throw "webui build failed" }
Pop-Location

Write-Host "[2/3] Checking PyInstaller ..."
python -m PyInstaller --version *> $null
if ($LASTEXITCODE -ne 0) {
    throw "PyInstaller missing. Run: pip install pyinstaller pywebview"
}

Write-Host "[3/3] Running PyInstaller ..."
$mode = if ($OneDir) { "--onedir" } else { "--onefile" }
$pyArgs = @(
    $mode, "--noconfirm", "--clean",
    "--add-data", "$root\webui\dist;webui\dist",
    "--add-data", "$root\assets;assets",
    # Model/livery DB used to build PI LIVERY; must be bundled or livery is empty.
    "--add-data", "$root\xfly_client\aircraft_db.b64;xfly_client",
    "--add-data", "$root\xfly_client\_aircraft_ids.b64;xfly_client",
    "--icon", "$root\assets\icon.ico",
    "--hidden-import", "webview.platforms.edgechromium",
    "--collect-all", "webview",
    "--name", "XFlySimClient",
    "--distpath", "$root\dist"
)
Push-Location $root
python -m PyInstaller @pyArgs --windowed webapp.py
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }
Pop-Location

Write-Host ""
Write-Host "Done. Output:"
Write-Host "  $root\dist\XFlySimClient.exe"
Write-Host "Config: binary file under %APPDATA%\XFlySim-Client\config.bin"
Write-Host "(machine-bound; auto-migrates legacy config.json on first run)"
Write-Host "Requires WebView2 Runtime (built into Windows 10/11)."