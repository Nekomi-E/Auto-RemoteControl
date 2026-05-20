@echo off
chcp 65001 >nul
cd /d "%~dp0"
setlocal

:: ============================================================
::  一键启动 Agent + Viewer（本机测试用）
::  用法: start_both.bat [port] [password] [quality] [fps]
::    port     - 端口 (默认: 27016)
::    password - 密码 (默认: test123)
::    quality  - 视频质量: 0=auto 1=balanced 2=lossless (默认: 2)
::    fps      - 目标帧率 (默认: 60)
:: ============================================================

set PORT=%1
if "%PORT%"=="" set PORT=27016
set PASS=%2
if "%PASS%"=="" set PASS=test123
set QUALITY=%3
if "%QUALITY%"=="" set QUALITY=2
set FPS=%4
if "%FPS%"=="" set FPS=60

set AGENT_EXE=build\Agent\Debug\Agent.exe
if not exist "%AGENT_EXE%" set AGENT_EXE=build\Agent\Release\Agent.exe
set VIEWER_EXE=build\Viewer\Debug\Viewer.exe
if not exist "%VIEWER_EXE%" set VIEWER_EXE=build\Viewer\Release\Viewer.exe

if not exist "%AGENT_EXE%" (
    echo [ERROR] Agent.exe not found. Build first: cmake --build build --config Debug
    pause
    exit /b 1
)
if not exist "%VIEWER_EXE%" (
    echo [ERROR] Viewer.exe not found. Build first: cmake --build build --config Debug
    pause
    exit /b 1
)

echo ============================================================
echo  一键启动: Agent + Viewer
echo ============================================================

:: Kill old instances
echo.
echo [INFO] Cleaning up old processes...
taskkill /F /IM Agent.exe  >nul 2>&1
taskkill /F /IM Viewer.exe >nul 2>&1
timeout /t 2 /nobreak >nul

:: Start Agent in background (same window)
echo [INFO] Starting Agent (background)...
start /B "Agent" "%AGENT_EXE%" --port %PORT% --password "%PASS%" --quality %QUALITY% --fps %FPS%

:: Wait for Agent to initialize
echo [INFO] Waiting for Agent to initialize (8s)...
timeout /t 8 /nobreak

:: Start Viewer in foreground
echo [INFO] Starting Viewer...
echo.
"%VIEWER_EXE%" --host 127.0.0.1 --port %PORT% --password "%PASS%"

echo.
echo [INFO] Both stopped.
pause
endlocal
