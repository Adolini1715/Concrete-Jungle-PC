@echo off
rem Double-click to record Predator: Concrete Jungle in xemu. Close xemu to stop.
cd /d "%~dp0"
xemu_soak.exe %*
pause
