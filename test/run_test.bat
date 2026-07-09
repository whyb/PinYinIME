@echo off
chcp 65001 >nul 2>&1
setlocal

set TEST_DIR=%~dp0..\build\Release\test\Release
set SERVICE=%TEST_DIR%\test_service.exe
set CLIENT=%TEST_DIR%\test_client.exe

echo ==========================================
echo   PinyinIME IPC Test Suite
echo ==========================================
echo.

:: Step 0: Kill any stale test service from previous run
taskkill /F /IM test_service.exe >nul 2>&1
timeout /T 1 /NOBREAK >nul

:: Step 1: Build
echo [1/4] Building test targets...
cmake --build %~dp0..\build --config Release --target test_service --target test_client --target test_partial
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed!
    exit /b 1
)
echo.

:: Step 2: Start test service
echo [2/4] Starting test dict service...
start "" /B "%SERVICE%" > "%TEMP%\test_service_out.txt" 2>&1
timeout /T 3 /NOBREAK >nul
echo.

:: Step 3: Run test client
echo [3/4] Running test client...
"%CLIENT%" -v
set TEST_RESULT=%ERRORLEVEL%
echo.

:: Step 4: Run quick dict verification
echo [4/4] Quick dict verification (test_partial)...
"%TEST_DIR%\test_partial.exe" nihao 2>&1 | findstr /C:"你"
if %ERRORLEVEL% EQU 0 (
    echo   ✅ dict lookup OK
) else (
    echo   ❌ dict lookup FAILED
)

:: Cleanup
echo.
echo Stopping test service...
taskkill /F /IM test_service.exe >nul 2>&1

echo.
if %TEST_RESULT% EQU 0 (
    echo ==========================================
    echo   ✅ All tests PASSED
    echo ==========================================
) else (
    echo ==========================================
    echo   ❌ Tests FAILED (exit code %TEST_RESULT%)
    echo ==========================================
)

exit /b %TEST_RESULT%
