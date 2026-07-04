@echo off
REM ozbot-re - play a full game vs 7 bots on q2dm1 while recording YOUR inputs.
REM
REM Launches a q2repro listen server (40Hz) you play in, with 7 bots as opposition and:
REM   - bot_inputlog 1  -> your per-frame usercmd trace to
REM                        ozbotre\logs\q2dm1_<timestamp>.jsonl  (only YOUR inputs are
REM                        logged, not the bots': forward/side/up, jump, attack, view
REM                        yaw/pitch, origin, velocity, speed, onground, waterlevel)
REM   - a synchronized .dm2 demo to ozbotre\demos\game_*.dm2  (for cross-check)
REM
REM Use it: run this, play a normal game -- roam the whole map, grab items, fight the
REM         bots. It ends on its own after the 10-minute timelimit (or type  quit  in
REM         the console (~) / use the menu). Quitting cleanly flushes the .dm2.
REM Then hand me the newest ozbotre\logs\q2dm1_*.jsonl and I'll mine it for map coverage,
REM playbook snippets, and humanization data.
REM
REM Video: runs fullscreen (vid_fullscreen 1, your native resolution). For a window
REM instead, change it to  +set vid_fullscreen 0 +set vid_geometry 1280x720
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"
cd /d "%Q2DIR%"

if not exist "ozbotre\gamex86_64.dll" (
  echo [ozbot-re] ozbotre\gamex86_64.dll not found under %Q2DIR%. Run build.bat + deploy.bat first.
  exit /b 1
)

set "DEMO=game_%RANDOM%"

REM cl_beginmapcmd starts the synced demo on map load; its value has a space so it goes
REM through a generated .cfg (command-line quoting through the batch is unreliable).
REM Everything else stays on the command line -- bot_count/map set via an exec'd cfg do
REM NOT spawn bots, but command-line +set + +map does.
> "ozbotre\record_game.cfg" (
  echo set cl_beginmapcmd "record %DEMO%"
)

echo.
echo [ozbot-re] 7 bots + you on q2dm1 at 40Hz (10-minute timelimit).
echo [ozbot-re] Input log : ozbotre\logs\q2dm1_^<timestamp^>.jsonl   (bot_inputlog 1)
echo [ozbot-re] Demo       : ozbotre\demos\%DEMO%.dm2
echo [ozbot-re] Play a full game (roam everywhere); it ends after 10 min or type  quit.
echo.

q2repro.exe +set com_rerelease -1 +set game ozbotre +set deathmatch 1 +set maxclients 16 ^
  +set sv_fps 40 +set bot_count 7 +set bot_skill 0.5 +set bot_inputlog 1 +set timelimit 10 ^
  +set vid_fullscreen 1 +exec record_game.cfg +map q2dm1
endlocal
