@echo off
rem Play Double Dash!!: double-click. Starts the game; the first time (or if it
rem isn't built yet) it builds it first, which takes a while.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-mkdd.ps1" -Play %*
if errorlevel 1 (
    echo.
    echo The build stopped. The message above says where; copy it for Claude.
    pause
)
