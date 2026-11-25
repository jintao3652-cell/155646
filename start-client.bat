@echo off
rem XFlySim Client launcher: double-click to start the RMP panel UI.
rem   start-client.bat          start normally (no console window)
rem   start-client.bat debug    start with DevTools in foreground
rem   start-client.bat check    check environment only
setlocal
cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-client.ps1" %*
if errorlevel 1 (
    echo.
    pause
)

endlocal
