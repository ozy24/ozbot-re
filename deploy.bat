@echo off
REM ozbot-re - copy the built DLL into the engine's ozbotre mod directory.
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal
cd /d "%~dp0"

if "%Q2DIR%"=="" set "Q2DIR=%~dp0engine"
if not exist "%Q2DIR%" (
  echo [ozbot-re] Q2DIR "%Q2DIR%" does not exist. Set Q2DIR to your Quake II install.
  exit /b 1
)
if not exist "dist\gamex86_64.dll" (
  echo [ozbot-re] dist\gamex86_64.dll not found. Run build.bat first.
  exit /b 1
)

if not exist "%Q2DIR%\ozbotre" mkdir "%Q2DIR%\ozbotre"
copy /y "dist\gamex86_64.dll" "%Q2DIR%\ozbotre\gamex86_64.dll" >nul
if errorlevel 1 (
  echo [ozbot-re] Copy failed.
  exit /b 1
)
echo [ozbot-re] Deployed gamex86_64.dll to %Q2DIR%\ozbotre
endlocal
