@echo off
rem Predator: Concrete Jungle PC: updater for v0.1 installs (v0.1 had no updater of its own).
rem Put this file in the folder you unzipped v0.1 into (the one with Install.bat), or in your
rem "Predator Concrete Jungle PC" game folder, and double-click it. It fetches the latest
rem release, keeps your saves, settings, texture packs and disc, and rebuilds the game.
setlocal
if /i not "%~1"=="--from-temp" (
  rem run from a copy: the installer moves the game folder, which may be where this file is
  copy /y "%~f0" "%TEMP%\pcj-update-v01.bat" >nul
  "%TEMP%\pcj-update-v01.bat" --from-temp "%~dp0"
  exit /b
)
set "HERE=%~2"
title Predator: Concrete Jungle PC updater
set "ROOT="
if exist "%HERE%installer\install.py" set "ROOT=%HERE%"
if not defined ROOT if exist "%HERE%..\installer\install.py" set "ROOT=%HERE%..\"
if not defined ROOT for /d %%d in ("%HERE%..\*") do if exist "%%d\installer\install.py" if exist "%%d\Install.bat" set "ROOT=%%d\"
if not defined ROOT goto notfound
cd /d "%ROOT%"
echo Updating the install in:
echo   %CD%
echo.
if exist "Update.bat" if exist "installer\update.py" goto hasupdater
if not exist "installer\python\tools\python.exe" goto nopython
echo Getting the updater ...
curl.exe -fsSL --retry 3 -o "installer\update.py" "https://raw.githubusercontent.com/Adolini1715/Concrete-Jungle-PC/main/installer/update.py"
if errorlevel 1 goto nodownload
"installer\python\tools\python.exe" -I "installer\update.py"
if errorlevel 3 goto uptodate
if errorlevel 1 goto failed
call "Install.bat"
exit /b

:hasupdater
echo This install already has its own updater. Running it.
call "Update.bat"
exit /b

:notfound
echo Could not find your Concrete Jungle install from here.
echo Put this file in the folder that has Install.bat in it, the one you unzipped
echo v0.1 into, and run it again.
echo.
echo If you deleted that folder: download the new release, unzip it in the same place
echo as before, next to your "Predator Concrete Jungle PC" folder, put your disc in
echo "PUT YOUR GAME HERE" and run its Install.bat. Your saves and settings are kept.
goto stop

:nopython
echo This folder never finished an install, so there is nothing to update.
echo Download the new release and run its Install.bat.
goto stop

:nodownload
echo Could not download the updater from GitHub. Check the internet connection
echo and run this file again.
goto stop

:failed
echo The update stopped. See update-log.txt in the folder named above.
goto stop

:uptodate
:stop
echo.
pause
exit /b 1
