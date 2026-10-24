@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0service.ps1" %*
exit /b %errorlevel%
