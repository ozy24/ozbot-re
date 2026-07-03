@echo off
REM ozbot-re - build the q2repro engine (x64): q2repro.exe (client) + q2reproded.exe (dedicated).
REM
REM The q2repro/ source tree carries local patches on the `ozbot-re` branch:
REM   - fastsim cvar (src/common/common.c): dedicated turbo mode, tick-rate aware
REM     (injects one server tick per loop iteration; 25ms at sv_fps 40)
REM   - variable-fps build fixes (the -Dvariable-fps=true option is off by default
REM     upstream and had bit-rotted)
REM Build is configured with -Dvariable-fps=true so classic game DLLs that export
REM GMF_VARIABLE_FPS can run at sv_fps 40.
REM
REM Needs: VS2022 C++ workload, meson+ninja (py -m pip install --user meson ninja),
REM        and initialized submodules (git submodule update --init --recursive).
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal enabledelayedexpansion
cd /d "%~dp0..\q2repro"

if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"

REM --- meson/ninja live in the pip user scripts dir; put it on PATH ---
for /f "usebackq tokens=*" %%i in (`py -c "import sysconfig; print(sysconfig.get_path('scripts','nt_user'))"`) do set "PYSCRIPTS=%%i"
if defined PYSCRIPTS set "PATH=%PYSCRIPTS%;%PATH%"
where meson >nul 2>nul
if errorlevel 1 (
  echo [ozbot-re] meson not found. Install with:  py -m pip install --user meson ninja
  exit /b 1
)

REM --- locate MSVC x64 toolchain if cl is not already on PATH ---
where cl >nul 2>nul
if %errorlevel%==0 goto havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ozbot-re] vswhere.exe not found; install Visual Studio with the "Desktop development with C++" workload.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo [ozbot-re] No Visual Studio installation found.
  exit /b 1
)
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo [ozbot-re] vcvarsall.bat not found at "%VCVARS%".
  exit /b 1
)
echo [ozbot-re] Initializing MSVC x64 environment...
call "%VCVARS%" x64
if errorlevel 1 (
  echo [ozbot-re] Failed to initialize the MSVC x64 environment.
  exit /b 1
)

:havecl
REM --- configure once ---
if not exist build-x64 (
  meson setup build-x64 -Dwrap_mode=forcefallback -Dvariable-fps=true ^
    -Davcodec=disabled -Dlibcurl=disabled -Dsdl2=disabled -Dlibjpeg=disabled ^
    -Dwindows-crash-dumps=disabled
  if errorlevel 1 exit /b 1
)

meson compile -C build-x64
if errorlevel 1 exit /b 1

copy /y "build-x64\q2repro.exe" "%Q2DIR%\q2repro.exe" >nul
copy /y "build-x64\q2reproded.exe" "%Q2DIR%\q2reproded.exe" >nul
copy /y "build-x64\baseq2\gamex86_64.dll" "%Q2DIR%\baseq2\gamex86_64.dll" >nul
copy /y "build-x64\baseq2\game_x64.dll" "%Q2DIR%\baseq2\game_x64.dll" >nul
if errorlevel 1 (
  echo [ozbot-re] Copy to "%Q2DIR%" failed.
  exit /b 1
)
echo [ozbot-re] Deployed q2repro.exe + q2reproded.exe + baseq2 game DLLs to %Q2DIR%
endlocal
