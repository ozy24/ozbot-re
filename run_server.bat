@echo off
REM ozbot - build, deploy, and launch a dedicated q2dm1 server with bots.
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
cd /d "%~dp0"

if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"

call build.bat
if errorlevel 1 exit /b 1
call deploy.bat
if errorlevel 1 exit /b 1

REM prefer q2pro, fall back to stock quake2
set "Q2EXE=%Q2DIR%\q2pro.exe"
if not exist "%Q2EXE%" set "Q2EXE=%Q2DIR%\quake2.exe"
if not exist "%Q2EXE%" (
  echo [ozbot] No q2pro.exe or quake2.exe found in %Q2DIR%.
  exit /b 1
)

echo.
echo [ozbot] Launching dedicated server with 2 bots on q2dm1.
echo [ozbot] In the server console you can record a demo with:
echo [ozbot]     serverrecord run1     ... and later ...     serverstop
echo [ozbot] Adjust bots live with:  bot_count 4   (or)   sv bot_add 2
echo.

"%Q2EXE%" +set dedicated 1 +set game ozbot +set deathmatch 1 +set maxclients 16 +set bot_count 2 +map q2dm1
endlocal
