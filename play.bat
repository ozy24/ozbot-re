@echo off
REM ozbot-re - launch a local game (q2repro listen server) so you can PLAY against the bots.
REM You spawn in q2dm1 with bots; adjust with the console (~) cvars below.
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"
cd /d "%Q2DIR%"

if not exist "ozbotre\gamex86_64.dll" (
  echo [ozbot-re] ozbotre\gamex86_64.dll not found under %Q2DIR%. Run build.bat + deploy.bat first.
  exit /b 1
)

REM com_rerelease -1: never auto-detect Steam/GoG installs -- stay hermetic in %Q2DIR%
REM sv_fps 40: the ozbot-re tick rate (without it the game runs the vanilla 10Hz
REM path and the recorded playbooks won't replay).
q2repro.exe +set com_rerelease -1 +set game ozbotre +set sv_fps 40 +set deathmatch 1 +set maxclients 16 ^
  +set bot_count 4 +set bot_skill 0.6 +map q2dm1
endlocal
