@echo off
REM ozbot-re - build gamex86_64.dll (64-bit) with MSVC.
REM The q2repro engine is built x64, so the classic game DLL must be x64 too
REM (engine looks for gamex86_64.dll; the 32-bit constraint died with q2pro).
setlocal enabledelayedexpansion
cd /d "%~dp0"

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
if not exist dist mkdir dist
if not exist build mkdir build
del /q build\*.obj >nul 2>nul

echo [ozbot-re] Compiling...
cl /nologo /c /MT /W3 /EHsc /O2 /Fobuild\ ^
   /D WIN32 /D NDEBUG /D _WINDOWS /D _CRT_SECURE_NO_WARNINGS /D C_ONLY ^
   src\*.c
if errorlevel 1 (
  echo [ozbot-re] Compile failed.
  exit /b 1
)

echo [ozbot-re] Linking...
link /nologo /subsystem:windows /dll /machine:X64 ^
   /def:src\game.def /out:dist\gamex86_64.dll build\*.obj ^
   kernel32.lib user32.lib winmm.lib
if errorlevel 1 (
  echo [ozbot-re] Link failed.
  exit /b 1
)

echo [ozbot-re] Built dist\gamex86_64.dll
endlocal
