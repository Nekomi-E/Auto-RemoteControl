@echo off
chcp 65001 >nul
setlocal

:: ============================================================
::  Viewer 快速启动脚本（主控端）
::  用法: start_viewer.bat [host] [port] [password]
::    host     - Agent 地址 (默认: 127.0.0.1)
::    port     - Agent 端口 (默认: 27016)
::    password - 连接密码 (默认: test123)
:: ============================================================

set VIEWER_HOST=%1
if "%VIEWER_HOST%"=="" set VIEWER_HOST=127.0.0.1
set VIEWER_PORT=%2
if "%VIEWER_PORT%"=="" set VIEWER_PORT=27016
set VIEWER_PASS=%3
if "%VIEWER_PASS%"=="" set VIEWER_PASS=test123

set VIEWER_EXE=build\Viewer\Debug\Viewer.exe
if not exist "%VIEWER_EXE%" set VIEWER_EXE=build\Viewer\Release\Viewer.exe

if not exist "%VIEWER_EXE%" (
    echo [ERROR] Viewer.exe not found. Please build first:
    echo   cmake --build build --config Debug
    pause
    exit /b 1
)

echo [INFO] Killing old Viewer processes...
taskkill /F /IM Viewer.exe >nul 2>&1
timeout /t 1 /nobreak >nul

echo [INFO] Starting Viewer connecting to %VIEWER_HOST%:%VIEWER_PORT%...
start "RemoteControl-Viewer" "%VIEWER_EXE%" --host %VIEWER_HOST% --port %VIEWER_PORT% --password "%VIEWER_PASS%"

echo [INFO] Viewer started.
endlocal
