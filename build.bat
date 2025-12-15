@echo off
setlocal enabledelayedexpansion
echo Building Quake 2 Mod Release Package...
echo.

REM Clean up any previous dist
if exist "dist" (
    echo Cleaning up previous dist...
    rmdir /s /q "dist"
)

REM Create dist directory structure
echo Creating dist directory structure...
mkdir "dist" 2>nul

REM Check for compiler (try GCC first, then MSVC)
where gcc.exe >nul 2>&1
if %errorlevel% equ 0 (
    echo Found GCC compiler.
    set USE_GCC=1
    goto :build_dll
)

REM Try to find MSVC
where cl.exe >nul 2>&1
if %errorlevel% equ 0 (
    echo Found MSVC compiler.
    set USE_GCC=0
    goto :build_dll
)

echo Attempting to find Visual Studio...
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    set USE_GCC=0
    goto :build_dll
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    set USE_GCC=0
    goto :build_dll
)

echo ERROR: No compiler found. Please install GCC or Visual Studio.
exit /b 1

:build_dll
echo.
echo Building 32-bit Quake 2 mod DLL...
echo.

REM Set script directory before changing directories
set SCRIPT_DIR=%~dp0

REM Verify src directory exists and has .c files
if not exist "%SCRIPT_DIR%src" (
    echo ERROR: src directory not found!
    echo Please copy required source files to src\ directory first.
    exit /b 1
)

dir /b "%SCRIPT_DIR%src\*.c" >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: No .c files found in src\ directory!
    echo Please copy required source files to src\ directory first.
    exit /b 1
)

REM Change to dist directory for building
cd dist

if %USE_GCC% equ 1 (
    REM Build with GCC - compile all .c files in src directory
    echo Compiling with GCC...
    REM Change to src directory to use wildcards, then back
    pushd "%SCRIPT_DIR%src"
    gcc -m32 -shared -o "%SCRIPT_DIR%dist\gamex86.dll" *.c ^
        -DWIN32 -D_WINDOWS -DNDEBUG -DC_ONLY ^
        -I"%SCRIPT_DIR%..\quake2-source\game" -I"%SCRIPT_DIR%..\quake2-source\qcommon" -I"%SCRIPT_DIR%src" ^
        -std=c99 -Wno-implicit-function-declaration -Wno-int-conversion ^
        -Wno-pointer-to-int-cast -Wno-incompatible-pointer-types ^
        -Wno-implicit-int -Wno-return-type -Wno-unused-variable -Wno-unused-function
    popd
    
    if %ERRORLEVEL% neq 0 (
        echo DLL build failed!
        cd ..
        exit /b 1
    )
) else (
    REM Build with MSVC
    echo Compiling with MSVC...
    set CFLAGS=/nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "C_ONLY" /c
    set INCLUDES=/I"%SCRIPT_DIR%..\quake2-source\game" /I"%SCRIPT_DIR%..\quake2-source\qcommon" /I"%SCRIPT_DIR%src"
    set LDFLAGS=/nologo /base:0x20000000 /subsystem:windows /dll /machine:I386
    set LIBS=kernel32.lib user32.lib winmm.lib
    
    REM Compile all .c files in src directory (output objects to dist)
    for %%f in ("%SCRIPT_DIR%src\*.c") do (
        cl.exe %CFLAGS% %INCLUDES% /Fo".\" "%%f"
        if %ERRORLEVEL% neq 0 (
            echo Compilation failed for %%f!
            cd ..
            exit /b 1
        )
    )
    
    REM Create .def file
    (
    echo EXPORTS
    echo     GetGameAPI
    ) > game.def
    
    REM Link the DLL
    link.exe %LDFLAGS% /def:game.def /out:"gamex86.dll" *.obj %LIBS%
    
    if %ERRORLEVEL% neq 0 (
        echo Linking failed!
        cd ..
        exit /b 1
    )
    
    REM Clean up intermediate files (object files and def file)
    del /q *.obj game.def 2>nul
)

if %ERRORLEVEL% equ 0 (
    echo 32-bit DLL build successful!
    
    cd ..
    
    REM Copy documentation
    echo Copying documentation...
    if exist "src\README.md" copy /Y "src\README.md" "dist\README.txt"
    
    REM Show dist contents
    echo.
    echo ==========================================
    echo Release package created successfully!
    echo ==========================================
    echo.
    echo Contents:
    dir /b "dist"
    echo.
    echo Release package ready in: dist\
    echo.
    echo To use: Copy gamex86.dll to your Quake 2 game directory
    echo.
    
    REM Copy DLL to game directory and launch (optional - customize paths as needed)
    REM Uncomment and modify these lines if you want auto-deploy:
    REM echo Copying DLL to game directory...
    REM copy /Y "dist\gamex86.dll" "e:\Tools\Quake2-Dev\bot\gamex86.dll"
    REM if %ERRORLEVEL% equ 0 (
    REM     echo Launching Quake 2...
    REM     start "" "e:\Tools\Quake2-Dev\q2pro.exe" +set game bot +set bot_debug 1 +map 2box4
    REM )
    
    exit /b 0
    
) else (
    echo DLL build failed!
    cd ..
    exit /b 1
)
