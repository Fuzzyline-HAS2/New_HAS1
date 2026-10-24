@echo off
setlocal
cd /d "%~dp0"
if not exist .venv\Scripts\python.exe (
  echo Run setup.cmd first.
  exit /b 1
)
.venv\Scripts\python.exe collector.py --config config.json
exit /b %errorlevel%
