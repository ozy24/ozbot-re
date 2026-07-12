@echo off
REM ozbot-re - build, deploy, and launch a dedicated q2dm1 server with bots (q2repro, 40Hz).
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
cd /d "%~dp0"

if "%Q2DIR%"=="" set "Q2DIR=%~dp0engine"

call build.bat
if errorlevel 1 exit /b 1
call deploy.bat
if errorlevel 1 exit /b 1

set "Q2EXE=%Q2DIR%\q2reproded.exe"
if not exist "%Q2EXE%" (
  echo [ozbot-re] q2reproded.exe not found in %Q2DIR%. Run build_engine.bat first.
  exit /b 1
)

echo.
echo [ozbot-re] Launching dedicated q2repro server with 2 bots on q2dm1.
echo [ozbot-re] Adjust bots live with:  bot_count 4   (or)   sv bot_add 2
echo.

REM com_rerelease -1: never auto-detect Steam/GoG installs -- stay hermetic in %Q2DIR%
"%Q2EXE%" +set com_rerelease -1 +set dedicated 1 +set game ozbotre +set sv_fps 40 +set deathmatch 1 +set maxclients 16 +set bot_count 2 +map q2dm1
endlocal
