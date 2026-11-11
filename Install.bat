@echo off
rem Builds Predator: Concrete Jungle for PC from your own copy of the game.
rem 1. Put your disc image (.iso) or extracted disc folder in "PUT YOUR GAME HERE".
rem 2. Double-click this file. The first run takes about an hour and needs the internet.
title Predator: Concrete Jungle PC installer
cd /d "%~dp0"
if not exist "%~dp0installer\bootstrap.ps1" (
  echo This folder is missing installer\bootstrap.ps1.
  echo Unzip the whole download first ^(right-click the zip, Extract All^), then run
  echo Install.bat from the unzipped folder.
  echo.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\bootstrap.ps1"
if errorlevel 1 (
  echo.
  echo The installer stopped. See install-log.txt in this folder.
) else (
  echo.
  echo All done. Start the game from the desktop shortcut or from the
  echo "Predator Concrete Jungle PC" folder inside this folder.
)
echo.
if defined PCJ_RELAUNCH if not errorlevel 1 exit /b 0
pause
