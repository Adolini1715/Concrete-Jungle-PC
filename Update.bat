@echo off
rem Updates Predator: Concrete Jungle PC to the latest release and rebuilds it.
rem Your saves, settings and texture packs in the game folder are kept.
rem
rem Everything runs inside the one block below. Windows reads a whole block before running it, so
rem the updater can replace this file while it runs. (Older versions ran a copy of this file from
rem the Temp folder instead, which Windows Defender flagged as a trojan.)
setlocal
cd /d "%~dp0"
title Predator: Concrete Jungle PC updater
(
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
  call "%~dp0Install.bat"
  exit /b
)
