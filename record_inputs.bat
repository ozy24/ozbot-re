@echo off
REM ozbot-re - record YOUR OWN inputs (+ a demo) on q2dm1 for jump analysis / playbooks.
REM
REM Launches a q2repro listen server you play in, with:
REM   - bot_inputlog 1  -> per-frame usercmd trace to ozbotre\logs\q2dm1_<timestamp>.jsonl
REM                        (forward/side/up move, jump, attack, view yaw/pitch, speed)
REM   - a synchronized .dm2 demo to ozbotre\demos\inputs_*.dm2 (for replay / cross-check)
REM
REM Use it:  run this, do the trick move (e.g. the Megahealth box+strafe jump) a few
REM          clean times, then type  quit  in the console (~) or use the menu to exit.
REM          (Quitting cleanly is what flushes the .dm2 -- don't just close the window.)
REM Then analyze with:  py tools\input_view.py <newest ozbotre\logs\q2dm1_*.jsonl>
REM or bake a playbook:  py tools\make_playbook.py <log.jsonl>   (Phase R4)
REM
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
if "%Q2DIR%"=="" set "Q2DIR=%~dp0engine"
cd /d "%Q2DIR%"

if not exist "ozbotre\gamex86_64.dll" (
  echo [ozbot-re] ozbotre\gamex86_64.dll not found under %Q2DIR%. Run build.bat + deploy.bat first.
  exit /b 1
)

set "DEMO=inputs_%RANDOM%"

REM Generate a config so the quoting for cl_beginmapcmd (starts the demo on map
REM load) is reliable -- command-line quoting through the batch is not.
> "ozbotre\record_inputs.cfg" (
  echo set deathmatch 1
  echo set maxclients 16
  echo set bot_count 0
  echo set bot_inputlog 1
  echo set vid_fullscreen 0
  echo set vid_geometry "1280x720"
  echo set cl_beginmapcmd "record %DEMO%"
  echo set sv_fps 40
  echo map q2dm1
)

echo.
echo [ozbot-re] Input log : ozbotre\logs\q2dm1_^<timestamp^>.jsonl   (bot_inputlog 1)
echo [ozbot-re] Demo       : ozbotre\demos\%DEMO%.dm2
echo [ozbot-re] Play q2dm1, do the trick moves, then type  quit  (or use the menu).
echo.

q2repro.exe +set com_rerelease -1 +set game ozbotre +exec record_inputs.cfg
endlocal
