@echo off
rem Updates Predator: Concrete Jungle PC to the latest release and rebuilds it.
rem Your saves, settings and texture packs in the game folder are kept.
setlocal
if /i not "%~1"=="--from-temp" (
  rem run from a copy, so the update may replace this file
  copy /y "%~f0" "%TEMP%\pcj-update.bat" >nul
  "%TEMP%\pcj-update.bat" --from-temp "%~dp0"
  exit /b
)
cd /d "%~2"
title Predator: Concrete Jungle PC updater
if not exist "installer\python\tools\python.exe" (
  echo Run Install.bat first: the updater uses what the installer set up.
  echo.
  pause
  exit /b 1
)
"installer\python\tools\python.exe" -I "installer\update.py"
if errorlevel 3 (
  echo.
  pause
  exit /b 0
)
if errorlevel 1 (
  echo.
  echo The update stopped. See update-log.txt in this folder.
  pause
  exit /b 1
)
call "%~2Install.bat"
