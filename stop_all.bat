@echo off

:: ============================================================
::  Stop all Agent and Viewer processes
:: ============================================================

echo [INFO] Stopping all RemoteControl processes...

taskkill /F /IM Agent.exe >nul 2>&1
if not errorlevel 1 (echo   Agent.exe stopped) else (echo   Agent.exe not running)

taskkill /F /IM Viewer.exe >nul 2>&1
if not errorlevel 1 (echo   Viewer.exe stopped) else (echo   Viewer.exe not running)

echo [INFO] Done.
