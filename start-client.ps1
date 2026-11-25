# XFlySim Client 启动器（RMP 面板 / pywebview 界面）
# 由 start-client.bat 调用：start-client.bat [start|debug|check]
param(
    [ValidateSet('start', 'debug', 'check')]
    [string]$Mode = 'start'
)

$ErrorActionPreference = 'Stop'
$root = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
Set-Location $root
$Host.UI.RawUI.WindowTitle = 'XFlySim Client'

function Write-Step($msg) { Write-Host "[*] $msg" }
function Write-Ok($msg)   { Write-Host "[ok] $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "[!] $msg" -ForegroundColor Yellow }
function Write-Err($msg)  { Write-Host "[x] $msg" -ForegroundColor Red }

# 候选解释器：优先 PATH 里的 python，其次 py 启动器，再退到常见安装位置。
# 这台机器上 PATH 里的 python 与装了 pywebview 的解释器可能不是同一个，所以要逐个试。
$candidates = @(
    @{ Exe = (Join-Path $env:LOCALAPPDATA 'Python\bin\python.exe'); Pre = @() }
    @{ Exe = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python313\python.exe'); Pre = @() }
    @{ Exe = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'); Pre = @() }
    @{ Exe = (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'); Pre = @() }
    @{ Exe = 'C:\Python313\python.exe'; Pre = @() }
    @{ Exe = 'C:\Python312\python.exe'; Pre = @() }
    @{ Exe = 'python'; Pre = @() }
    @{ Exe = 'py';     Pre = @('-3') }
)

# 解析成真实可执行文件；跳过 Microsoft Store 的 App Execution Alias
# （C:\...\WindowsApps\python.exe，直接跑会去拉商店，不是真解释器）
function Resolve-Exe($c) {
    if ($c.Exe -match '[\\/]') {
        if (Test-Path $c.Exe) { return (Resolve-Path $c.Exe).Path }
        return $null
    }
    $cmd = Get-Command $c.Exe -ErrorAction SilentlyContinue
    if (-not $cmd) { return $null }
    if ($cmd.Source -like '*\WindowsApps\*') { return $null }
    return $cmd.Source
}

function Test-Interpreter($c) {
    if (-not (Resolve-Exe $c)) { return $false }
    try {
        & $c.Exe @($c.Pre + @('-c', 'import sys')) *> $null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    }
}

function Test-Module($c, $module) {
    try {
        & $c.Exe @($c.Pre + @('-c', "import $module")) *> $null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    }
}

function Get-ExePath($c) {
    $p = Resolve-Exe $c
    if ($p) { return $p }
    return $c.Exe
}

# ---------- 1. 找 Python ----------
$py = $null
$fallback = $null
foreach ($c in $candidates) {
    if (-not (Test-Interpreter $c)) { continue }
    if (-not $fallback) { $fallback = $c }
    if (Test-Module $c 'webview') { $py = $c; break }
}

if (-not $py) {
    if (-not $fallback) {
        Write-Err '没有找到可用的 Python。'
        Write-Host '    请安装 Python 3.10+，安装时勾选 "Add python.exe to PATH"。'
        exit 1
    }
    if ($Mode -eq 'check') {
        Write-Warn '当前 Python 缺少 pywebview（正常启动时会自动安装）。'
        Write-Host "    解释器：$($fallback.Exe)"
        exit 1
    }
    Write-Warn '当前 Python 缺少 pywebview，正在安装依赖 ...'
    & $fallback.Exe @($fallback.Pre + @('-m', 'pip', 'install', '-r', 'requirements.txt'))
    if ($LASTEXITCODE -ne 0 -or -not (Test-Module $fallback 'webview')) {
        Write-Err '依赖安装失败，请手动执行：'
        Write-Host "    $($fallback.Exe) -m pip install -r requirements.txt"
        exit 1
    }
    $py = $fallback
}

$pythonExe = Get-ExePath $py

# ---------- 2. 前端构建产物 ----------
$dist = Join-Path $root 'webui\dist\index.html'
if (-not (Test-Path $dist)) {
    Write-Err '未找到前端构建产物：webui\dist\index.html'
    Write-Host '    首次运行请先构建界面：'
    Write-Host '        cd webui'
    Write-Host '        npm install'
    Write-Host '        npm run build'
    exit 1
}

# ---------- 3. 无控制台的解释器 ----------
$pythonw = Join-Path (Split-Path $pythonExe) 'pythonw.exe'
if (-not (Test-Path $pythonw)) { $pythonw = $pythonExe }

if ($Mode -eq 'check') {
    Write-Ok "Python   : $pythonExe"
    Write-Ok "无控制台 : $pythonw"
    Write-Ok '界面文件 : webui\dist\index.html'
    Write-Ok "工作目录 : $root"
    Write-Host ''
    Write-Host '环境检查通过，直接双击 start-client.bat 即可启动。'
    exit 0
}

# ---------- 4. 启动 ----------
if ($Mode -eq 'debug') {
    Write-Step '以调试模式启动（DevTools 可用）...'
    & $pythonExe webapp.py --debug
    exit $LASTEXITCODE
}

Write-Step '正在启动 XFlySim Client ...'
$proc = Start-Process -FilePath $pythonw -ArgumentList 'webapp.py' -WorkingDirectory $root -PassThru

# 无控制台启动时错误会被吞掉，这里盯 3 秒：如果立刻异常退出，就改前台跑一遍把报错显示出来
Start-Sleep -Seconds 3
if ($proc -and $proc.HasExited -and $proc.ExitCode -ne 0) {
    Write-Err "启动后立即退出（exit code $($proc.ExitCode)），下面用前台方式重跑以显示错误："
    Write-Host ''
    & $pythonExe webapp.py
    exit 1
}

Write-Ok "已启动（PID $($proc.Id)）"
exit 0
