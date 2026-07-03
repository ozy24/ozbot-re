@echo off
REM ozbot - copy the built DLL into the engine's ozbot mod directory.
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
cd /d "%~dp0"

if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"
if not exist "%Q2DIR%" (
  echo [ozbot] Q2DIR "%Q2DIR%" does not exist. Set Q2DIR to your Quake II install.
  exit /b 1
)
if not exist "dist\gamex86.dll" (
  echo [ozbot] dist\gamex86.dll not found. Run build.bat first.
  exit /b 1
)

if not exist "%Q2DIR%\ozbot" mkdir "%Q2DIR%\ozbot"
copy /y "dist\gamex86.dll" "%Q2DIR%\ozbot\gamex86.dll" >nul
if errorlevel 1 (
  echo [ozbot] Copy failed.
  exit /b 1
)
echo [ozbot] Deployed gamex86.dll to %Q2DIR%\ozbot
endlocal
