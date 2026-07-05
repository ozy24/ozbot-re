@echo off
REM ozbot - build, deploy, then run N headless sims in parallel and analyze.
REM Forwards all args to tools/run_parallel.py, e.g.:
REM     run_parallel.bat --instances 8 --seconds 90
REM     run_parallel.bat --instances 4 --seconds 60 --bots 5 --timescale 2
setlocal
cd /d "%~dp0"

if "%Q2DIR%"=="" set "Q2DIR=%~dp0..\engine"

call build.bat
if errorlevel 1 exit /b 1
call deploy.bat
if errorlevel 1 exit /b 1

REM prefer the py launcher (python.exe can resolve to the Store stub here)
where py >nul 2>&1 && (set "PY=py") || (set "PY=python")

echo.
echo [ozbot] Running parallel sims via tools\run_parallel.py %*
echo.
%PY% "%~dp0tools\run_parallel.py" --engine "%Q2DIR%" %*
endlocal
