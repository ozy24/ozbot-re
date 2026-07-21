@echo off
REM ozbot-re - launch a q2dm1 listen server for granular human-input recording.
REM
REM This just starts the server; recording itself is driven in-game with the
REM  "capture" console command (~ to open console):
REM    capture start <name>   -> begins ozbotre\logs\<name>.jsonl (per-frame
REM                              usercmd trace: forward/side/up move, jump,
REM                              attack, view yaw/pitch, speed) AND a synced
REM                              ozbotre\demos\<name>.dm2 demo
REM    capture stop           -> ends both, reports frame count
REM    capture status         -> shows whether a take is in progress
REM Repeat start/stop with new names to carve many clean, well-labelled takes
REM (e.g. the Megahealth box+strafe jump) in a single session, no relaunch
REM needed.  A take with a name already used on disk is OVERWRITTEN.
REM
REM Analyze with:  py tools\input_view.py ozbotre\logs\<name>.jsonl
REM or bake a playbook:  py tools\make_playbook.py ozbotre\logs\<name>.jsonl
REM
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
if "%Q2DIR%"=="" set "Q2DIR=%~dp0engine"
cd /d "%Q2DIR%"

if not exist "ozbotre\gamex86_64.dll" (
  echo [ozbot-re] ozbotre\gamex86_64.dll not found under %Q2DIR%. Run build.bat + deploy.bat first.
  exit /b 1
)

> "ozbotre\record_inputs.cfg" (
  echo set deathmatch 1
  echo set maxclients 16
  echo set bot_count 0
  echo set vid_fullscreen 0
  echo set vid_geometry "1280x720"
  echo set sv_fps 40
  echo map q2dm1
)

echo.
echo [ozbot-re] Listen server up. In the console (~):
echo [ozbot-re]   capture start ^<name^>   -- begin logs\^<name^>.jsonl + demos\^<name^>.dm2
echo [ozbot-re]   capture stop            -- end the take, reports frame count
echo [ozbot-re]   capture status          -- check if a take is in progress
echo [ozbot-re] Repeat with new names for as many takes as you like, then quit.
echo.

q2repro.exe +set com_rerelease -1 +set game ozbotre +exec record_inputs.cfg
endlocal
