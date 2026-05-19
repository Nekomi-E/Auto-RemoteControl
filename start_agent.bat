@echo off
chcp 65001 >nul
setlocal

:: ============================================================
::  Agent 快速启动脚本（被控端）
::  用法: start_agent.bat [port] [password]
::    port     - 监听端口 (默认: 27016)
::    password - 连接密码 (默认: test123)
:: ============================================================

set AGENT_PORT=%1
if "%AGENT_PORT%"=="" set AGENT_PORT=27016
set AGENT_PASS=%2
if "%AGENT_PASS%"=="" set AGENT_PASS=test123

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

echo [INFO] Starting Agent on port %AGENT_PORT% with password "%AGENT_PASS%"...
start "RemoteControl-Agent" "%AGENT_EXE%" --port %AGENT_PORT% --password "%AGENT_PASS%"

echo [INFO] Agent started. Check the Agent window for logs.
endlocal
