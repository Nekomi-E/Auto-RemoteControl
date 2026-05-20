@echo off
cd /d "%~dp0"
setlocal

set AGENT_PORT=%1
if "%AGENT_PORT%"=="" set AGENT_PORT=27016
set AGENT_PASS=%2
if "%AGENT_PASS%"=="" set AGENT_PASS=test123
set AGENT_QUALITY=%3
if "%AGENT_QUALITY%"=="" set AGENT_QUALITY=2
set AGENT_FPS=%4
if "%AGENT_FPS%"=="" set AGENT_FPS=60

set AGENT_EXE=build\Agent\Debug\Agent.exe
if not exist "%AGENT_EXE%" set AGENT_EXE=build\Agent\Release\Agent.exe

if not exist "%AGENT_EXE%" (
    echo [ERROR] Agent.exe not found. Please build first:
    echo   cmake --build build --config Debug
    pause
    exit /b 1
)

echo [INFO] Killing old Agent processes...
taskkill /F /IM Agent.exe >nul 2>&1
timeout /t 1 /nobreak >nul

echo [INFO] Starting Agent on port %AGENT_PORT%...
echo.
"%AGENT_EXE%" --port %AGENT_PORT% --password "%AGENT_PASS%" --quality %AGENT_QUALITY% --fps %AGENT_FPS%

pause
endlocal
