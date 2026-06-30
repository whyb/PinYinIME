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
cl.exe /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE main.cpp trie_dict.cpp /Fe:PinyinIME.exe user32.lib gdi32.lib kernel32.lib imm32.lib comctl32.lib shell32.lib oleacc.lib Ole32.lib OleAut32.lib UIAutomationCore.lib

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo [2/2] Copying rime-ice dictionaries...
if exist cn_dicts rmdir /s /q cn_dicts
xcopy /e /i /q rime-ice\cn_dicts cn_dicts >nul
echo   Done: cn_dicts\ copied to output

echo.
echo ==========================================
echo   Build OK: PinyinIME.exe
echo ==========================================
echo.
echo Usage:
echo   1. Run PinyinIME.exe (as Administrator)
echo   2. Ctrl+Shift to toggle Chinese/English
echo   3. Type pinyin, Space to confirm, 1-9 to select
echo.
