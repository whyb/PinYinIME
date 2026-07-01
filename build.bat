@echo off
chcp 65001 >nul 2>&1

echo ==========================================
echo   PinyinIME Build Script (CMake + MSVC)
echo ==========================================
echo.

where cmake.exe >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] cmake.exe not found!
    echo Please install CMake and run this in VS2022 Developer Command Prompt.
    pause
    exit /b 1
)

echo [1/2] Configuring CMake...
cmake -B build -G "Visual Studio 17 2022" -A x64
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configure failed!
    pause
    exit /b 1
)

echo.
echo [2/2] Building Release...
cmake --build build --config Release
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo ==========================================
echo   Build OK ^^!
echo   DLL: build\Release\PinyinIMETSF.dll
echo   EXE: build\Release\PinyinIME.exe
echo ==========================================
echo.
echo Usage:
echo   1. Run build\Release\PinyinIME.exe
echo   2. Open Settings -^> click "Register IME to System"
echo   3. Add PinyinIME in Windows Language Settings
echo   4. Use Win+Space to switch to PinyinIME
echo.
