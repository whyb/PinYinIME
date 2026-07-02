@echo off
echo ============================================
echo   PinyinIME TSF IME - Register to System
echo ============================================
echo.
echo This script registers PinyinIME as a Windows TSF text service.
echo Steps:
echo   1. COM component registration (DllRegisterServer)
echo   2. TSF framework registration (ITfInputProcessorProfileMgr)
echo   3. Auto-start registration (HKCU Run)
echo.

REM Check admin privilege (fltmc requires admin)
fltmc >nul 2>&1
if %errorlevel% neq 0 (
    echo [INFO] Admin privilege required, requesting UAC elevation...
    powershell -Command "Start-Process '%~dp0PinyinIME.exe' -ArgumentList '--register-system' -Verb RunAs -Wait"
) else (
    echo [INFO] Already running as admin, registering...
    "%~dp0PinyinIME.exe" --register-system
)

echo.
echo ============================================
echo   Done! Add the keyboard in Language settings:
echo   Settings -^> Language -^> Chinese -^> Keyboard -^> Add -^> PinyinIME
echo ============================================
pause
