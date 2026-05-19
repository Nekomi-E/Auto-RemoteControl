@echo off
chcp 65001 >nul

:: ============================================================
::  一键启动 Agent + Viewer（本机测试用）
::  用法: start_both.bat [port] [password]
:: ============================================================

set PORT=%1
if "%PORT%"=="" set PORT=27016
set PASS=%2
if "%PASS%"=="" set PASS=test123

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
echo  一键启动: Agent 监听 :%PORT%  +  Viewer 连接 127.0.0.1:%PORT%
echo ============================================================

:: Kill old instances
echo.
echo [INFO] Cleaning up old processes...
taskkill /F /IM Agent.exe  >nul 2>&1
taskkill /F /IM Viewer.exe >nul 2>&1
timeout /t 2 /nobreak >nul

:: Start Agent
echo [INFO] Starting Agent...
start "RemoteControl-Agent" cmd /c "cd /d "%~dp0" && "%AGENT_EXE%" --port %PORT% --password "%PASS%""

:: Wait for Agent to initialize (capture + encoder takes ~3-5s)
echo [INFO] Waiting for Agent to initialize (5s)...
timeout /t 5 /nobreak >nul

:: Start Viewer
echo [INFO] Starting Viewer...
start "RemoteControl-Viewer" cmd /c "cd /d "%~dp0" && "%VIEWER_EXE%" --host 127.0.0.1 --port %PORT% --password "%PASS%""

echo.
echo [INFO] Both started. Use stop_all.bat to stop.
echo ============================================================
