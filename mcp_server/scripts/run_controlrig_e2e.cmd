@echo off
setlocal
set SCRIPT_DIR=%~dp0
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%run_controlrig_e2e.ps1" %*
exit /b %ERRORLEVEL%
