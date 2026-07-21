@echo off
REM Build the standalone nav generator -> dist\gen_nav.exe (no Python needed to run).
REM Requires PyInstaller:  py -m pip install pyinstaller
setlocal
cd /d "%~dp0"
py -m PyInstaller --noconfirm --clean ^
    --distpath dist --workpath build\gen_nav ^
    tools\gen_nav.spec
if errorlevel 1 (
    echo [build_gen_nav] FAILED
    exit /b 1
)
echo [build_gen_nav] Built dist\gen_nav.exe
