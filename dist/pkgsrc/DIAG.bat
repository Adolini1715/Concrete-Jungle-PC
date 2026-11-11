@echo off
rem Predator: Concrete Jungle - 90-second diagnostic run.
rem Starts the game, lets it run on its own for 90 seconds (no need to touch
rem anything), then closes it so the session log and profile are complete.
rem The results land in the newest logs\session-... folder next to pcj.exe.
cd /d "%~dp0"
if not exist "C:\Games\PCJ\game\DEFAULT.XBE" (
  echo Run PLAY.bat once first so the game files get unpacked.
  pause
  exit /b 1
)
echo Running the game for 90 seconds to record a performance profile...
set CJ_PROF_CHAINS=1
start "" pcj.exe
timeout /t 90 /nobreak >nul
taskkill /im pcj.exe >nul 2>&1
timeout /t 5 /nobreak >nul
taskkill /f /im pcj.exe >nul 2>&1
echo Done. The results are in the newest folder under "logs".
timeout /t 5 >nul
