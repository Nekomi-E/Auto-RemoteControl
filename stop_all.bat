@echo off
chcp 65001 >nul

:: ============================================================
::  停止所有 Agent 和 Viewer 进程
:: ============================================================

echo [INFO] Stopping all RemoteControl processes...

taskkill /F /IM Agent.exe >nul 2>&1
if %errorlevel% equ 0 (echo   Agent.exe stopped) else (echo   Agent.exe not running)

taskkill /F /IM Viewer.exe >nul 2>&1
if %errorlevel% equ 0 (echo   Viewer.exe stopped) else (echo   Viewer.exe not running)

echo [INFO] Done.
