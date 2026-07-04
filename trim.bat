@echo off
REM ozbot-re - trim a recording made by record.bat into a short clip.
REM
REM   trim.bat <input.mp4> <start_seconds> <duration_seconds> [output.mp4]
REM
REM Example (10s clip starting 92s in, e.g. 5s before an MH jump + the jump):
REM   trim.bat recordings\q2dm1_20260704_110000.mp4 92 10 mh_jump.mp4
REM
REM Re-encodes for a frame-accurate cut.  Output defaults to <input>_clip.mp4.
setlocal
if "%~3"=="" (
  echo Usage: trim.bat ^<input.mp4^> ^<start_seconds^> ^<duration_seconds^> [output.mp4]
  echo   e.g. trim.bat recordings\q2dm1_20260704_110000.mp4 92 10 mh_jump.mp4
  exit /b 1
)
set "IN=%~1"
set "START=%~2"
set "DUR=%~3"
set "OUT=%~4"
if "%OUT%"=="" set "OUT=%~dpn1_clip.mp4"

set "FFMPEG="
where ffmpeg >nul 2>nul && set "FFMPEG=ffmpeg"
if not defined FFMPEG for /f "delims=" %%f in ('dir /b /s "%LOCALAPPDATA%\Microsoft\WinGet\Packages\ffmpeg.exe" 2^>nul ^| findstr /i "Gyan"') do set "FFMPEG=%%f"
if not defined FFMPEG ( echo [ozbot-re] ffmpeg not found. Install with: winget install Gyan.FFmpeg & exit /b 1 )

"%FFMPEG%" -y -ss %START% -i "%IN%" -t %DUR% -c:v libx264 -preset veryfast -pix_fmt yuv420p -an "%OUT%"
if errorlevel 1 ( echo [ozbot-re] trim failed. & exit /b 1 )
echo [ozbot-re] Wrote %OUT%
endlocal
