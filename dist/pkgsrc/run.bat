@echo off
rem Predator: Concrete Jungle - PC test build launcher.
rem The game folder comes from pcj.ini (game=...), or pass it here:
rem   run.bat "C:\path\to\Predator Concrete Jungle"
rem Everything the game prints goes to pcj-log.txt next to pcj.exe.
cd /d "%~dp0"
pcj.exe %*
if errorlevel 1 (
  echo.
  echo pcj.exe exited with an error. Send pcj-log.txt from this folder.
  pause
)
