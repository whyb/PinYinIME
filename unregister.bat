@echo off
echo ============================================
echo   PinyinIME TSF IME - Unregister from System
echo ============================================
echo.
echo This script removes PinyinIME from the Windows TSF framework.
echo Steps:
echo   1. TSF framework unregistration (UnregisterProfile)
echo   2. COM registry cleanup (DllUnregisterServer)
echo   3. Auto-start removal
echo   4. Terminate processes holding the DLL (requires confirmation)
echo.

REM Check admin privilege (fltmc requires admin)
fltmc >nul 2>&1
if %errorlevel% neq 0 (
    echo [INFO] Admin privilege required, requesting UAC elevation...
    powershell -Command "Start-Process '%~dp0PinyinIME.exe' -ArgumentList '--unregister-system' -Verb RunAs -Wait"
) else (
    echo [INFO] Already running as admin, unregistering...
    "%~dp0PinyinIME.exe" --unregister-system
)

echo.
echo ============================================
echo   Done! If the DLL is still locked, restart
echo   Windows before replacing the DLL file.
echo ============================================
pause
