@echo off
rem Predator: Concrete Jungle - play and record the session for exact replay.
rem The game runs in its deterministic mode: every frame's timing is fixed and
rem the controller is recorded, so the session can be replayed exactly (in the
rem cloud, for crash and regression testing). The recording goes to
rem logs\rec-<date>-<time> next to this file (a few MB per hour).
rem Known for now: the pre-rendered movies play without sound in this mode.
cd /d "%~dp0"
if not exist "C:\Games\PCJ\game\DEFAULT.XBE" (
  echo Run PLAY.bat once first so the game files get unpacked.
  pause
  exit /b 1
)
if not exist "C:\Games\PCJ\save" mkdir "C:\Games\PCJ\save"
set CJ_REC=1
pcj.exe
if errorlevel 1 (
  echo.
  echo pcj.exe exited with an error. Send pcj-log.txt from this folder.
  pause
)
