@echo off
REM ozbot - build the patched fastsim dedicated engine (q2proded_fast.exe, 32-bit).
REM
REM The q2pro/ source tree carries a small local patch (src/common/common.c):
REM a `fastsim` cvar that makes the dedicated server loop skip its per-tick
REM sleep and inject exactly one game tick per loop iteration, so the sim runs
REM CPU-bound (~hundreds of x realtime) instead of sleeping ~95% of the time.
REM Used by tools/run_parallel.py --fastsim.  Must be x86: the game DLL is 32-bit.
REM
REM Needs: VS2022 C++ workload, and meson+ninja (py -m pip install --user meson ninja).
REM Override the install location with:  set Q2DIR=C:\path\to\quake2
setlocal enabledelayedexpansion
cd /d "%~dp0..\q2pro"

if "%Q2DIR%"=="" set "Q2DIR=E:\Projects\ozbot\engine"

REM --- meson/ninja live in the pip user scripts dir; put it on PATH ---
for /f "usebackq tokens=*" %%i in (`py -c "import sysconfig; print(sysconfig.get_path('scripts','nt_user'))"`) do set "PYSCRIPTS=%%i"
if defined PYSCRIPTS set "PATH=%PYSCRIPTS%;%PATH%"
where meson >nul 2>nul
if errorlevel 1 (
  echo [ozbot] meson not found. Install with:  py -m pip install --user meson ninja
  exit /b 1
)

REM --- locate MSVC x86 toolchain if cl is not already on PATH ---
where cl >nul 2>nul
if %errorlevel%==0 goto havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ozbot] vswhere.exe not found; install Visual Studio with the "Desktop development with C++" workload.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo [ozbot] No Visual Studio installation found.
  exit /b 1
)
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo [ozbot] vcvarsall.bat not found at "%VCVARS%".
  exit /b 1
)
echo [ozbot] Initializing MSVC x86 environment...
call "%VCVARS%" x86
if errorlevel 1 (
  echo [ozbot] Failed to initialize the MSVC x86 environment.
  exit /b 1
)

:havecl
REM --- configure once (dedicated-only: every optional dep disabled) ---
if not exist build-fast-x86 (
  meson setup build-fast-x86 -Dwrap_mode=nofallback -Dzlib=disabled -Dlibcurl=disabled ^
    -Dsdl2=disabled -Dopenal=disabled -Dlibpng=disabled -Davcodec=disabled ^
    -Dwindows-crash-dumps=disabled
  if errorlevel 1 exit /b 1
)

meson compile -C build-fast-x86 q2proded
if errorlevel 1 exit /b 1

copy /y "build-fast-x86\q2proded.exe" "%Q2DIR%\q2proded_fast.exe" >nul
if errorlevel 1 (
  echo [ozbot] Copy to "%Q2DIR%" failed.
  exit /b 1
)
echo [ozbot] Deployed q2proded_fast.exe to %Q2DIR%
endlocal
