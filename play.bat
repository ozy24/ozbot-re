@echo off
REM ozbot - launch a local game (listen server) so you can PLAY against the bots.
REM You spawn in q2dm1 with bots; adjust with the console (~) cvars below.
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"
cd /d "%Q2DIR%"

if not exist "ozbot\gamex86.dll" (
  echo [ozbot] ozbot\gamex86.dll not found under %Q2DIR%. Run build.bat + deploy.bat first.
  exit /b 1
)

q2pro.exe +set game ozbot +set deathmatch 1 +set maxclients 16 ^
  +set bot_count 4 +set bot_skill 0.6 +map q2dm1
endlocal
