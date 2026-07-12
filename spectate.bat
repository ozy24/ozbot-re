@echo off
REM ozbot-re - launch a q2repro listen server and join as a SPECTATOR (chase cam).
REM Bots fight on q2dm1 at 40Hz; you watch from the sidelines. Console (~):
REM   Fire = enter/exit chase cam    Jump = next target    Prev weapon = prev target
REM   Use  = toggle eyecam / third-person while chasing
REM   bot_debug 1  = draw nav paths and combat debug beams
REM With sv_fps 40 the recorded playbooks fire, so you'll see bots do the
REM Megahealth jump and the Combat-Armor box jump (they need 40Hz to replay).
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
if "%Q2DIR%"=="" set "Q2DIR=%~dp0engine"
cd /d "%Q2DIR%"

if not exist "ozbotre\gamex86_64.dll" (
  echo [ozbot-re] ozbotre\gamex86_64.dll not found under %Q2DIR%. Run build.bat + deploy.bat first.
  exit /b 1
)

REM com_rerelease -1: stay hermetic in %Q2DIR% (don't hijack a Steam/GoG install).
REM sv_fps 40: the ozbot-re tick rate -- REQUIRED for the playbooks to replay.
q2repro.exe +set com_rerelease -1 +set game ozbotre +set sv_fps 40 +set deathmatch 1 +set maxclients 16 ^
  +set bot_count 6 +set bot_skill 0.6 +set spectator 1 +map q2dm1
endlocal
