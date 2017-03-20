@echo off
chcp 65001 >nul 2>&1

echo ==========================================
echo   PinyinIME Build Script
echo ==========================================
echo.

where cl.exe >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] cl.exe not found!
    echo Please run this in VS2022 Developer Command Prompt.
    pause
    exit /b 1
)

echo [1/2] Compiling...
cl.exe /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE main.cpp /Fe:PinyinIME.exe user32.lib gdi32.lib kernel32.lib imm32.lib

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo [2/2] Cleaning...
if exist main.obj del main.obj

echo.
echo ==========================================
echo   Build OK: PinyinIME.exe
echo ==========================================
echo.
echo Usage:
echo   1. Run PinyinIME.exe
echo   2. Right Shift to toggle Chinese/English
echo   3. Type pinyin, Space to confirm, 1-9 to select
echo.
