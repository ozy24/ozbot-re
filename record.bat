@echo off
REM ozbot-re - record gameplay to an .mp4 while you play / spectate q2dm1.
REM
REM Opens a windowed q2repro listen server (you join as a SPECTATOR so you can
REM chase-cam the bots) AND an ffmpeg window that screen-records the game.
REM
REM   1. Running this pops up the GAME window and an FFMPEG console window.
REM   2. In the game (console = ~):  Fire = chase a bot / exit chase,
REM      Jump = next bot, Prev-weapon = previous bot, Use = eyecam toggle.
REM      Line up and watch the moment you want (e.g. a bot doing the Megahealth
REM      jump -- they attempt it every ~30-40s with bot_count 6).
REM   3. When done, click the FFMPEG window and press  q  to stop and save.
REM      (Press q -- do NOT just close it, or the .mp4 won't be finalized.)
REM   4. Trim the clip afterwards with:  trim.bat <file> <start_s> <dur_s>
REM
REM Output: ozbot-re\recordings\q2dm1_<timestamp>.mp4  (full 1280x720 window)
REM Needs the DLL deployed first (build.bat + deploy.bat) and ffmpeg installed
REM   (winget install Gyan.FFmpeg).  Override install dir with:  set Q2DIR=...
setlocal enabledelayedexpansion

if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"

if not exist "%Q2DIR%\ozbotre\gamex86_64.dll" (
  echo [ozbot-re] ozbotre\gamex86_64.dll not found under %Q2DIR%. Run build.bat + deploy.bat first.
  exit /b 1
)

REM --- locate ffmpeg (PATH first, else the winget install location) ---
set "FFMPEG="
where ffmpeg >nul 2>nul && set "FFMPEG=ffmpeg"
if not defined FFMPEG for /f "delims=" %%f in ('dir /b /s "%LOCALAPPDATA%\Microsoft\WinGet\Packages\ffmpeg.exe" 2^>nul ^| findstr /i "Gyan"') do set "FFMPEG=%%f"
if not defined FFMPEG (
  echo [ozbot-re] ffmpeg not found. Install it with:  winget install Gyan.FFmpeg
  exit /b 1
)

REM --- timestamped output file ---
set "OUTDIR=%~dp0recordings"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
for /f %%t in ('powershell -NoProfile -Command "(Get-Date).ToString('yyyyMMdd_HHmmss')"') do set "STAMP=%%t"
set "MP4=%OUTDIR%\q2dm1_!STAMP!.mp4"

REM --- launch the game: windowed spectator listen server, bots on q2dm1, 40Hz ---
cd /d "%Q2DIR%"
start "" q2repro.exe +set com_rerelease -1 +set game ozbotre +set deathmatch 1 +set maxclients 16 +set sv_fps 40 +set bot_count 6 +set spectator 1 +set vid_fullscreen 0 +set vid_geometry 1280x720 +map q2dm1

echo.
echo [ozbot-re] Waiting for the game window...
REM wait until the q2repro window exists (up to ~15s) so gdigrab can find it
powershell -NoProfile -Command "for($i=0;$i -lt 50;$i++){$p=Get-Process q2repro -ErrorAction SilentlyContinue; if($p -and $p.MainWindowHandle -ne 0){break}; Start-Sleep -Milliseconds 300}"

echo [ozbot-re] Recording the game window to:
echo     !MP4!
echo [ozbot-re] Play / spectate, then press  q  in the ffmpeg window to STOP and save.
echo.

REM ffmpeg in its own window so you can press q to finalize the mp4 cleanly
start "ffmpeg -- click here and press q to STOP recording" "%FFMPEG%" -y -f gdigrab -framerate 30 -i title=Q2REPRO -c:v libx264 -preset veryfast -pix_fmt yuv420p "!MP4!"

endlocal
