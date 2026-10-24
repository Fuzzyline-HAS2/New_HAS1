@echo off
setlocal
cd /d "%~dp0"
py -3 -c "import sys; assert sys.version_info >= (3,11), 'Python 3.11 or newer required'"
if errorlevel 1 exit /b 1
py -3 -m venv .venv
if errorlevel 1 exit /b 1
.venv\Scripts\python.exe -m pip install --requirement requirements.txt
if errorlevel 1 exit /b 1
if not exist config.json copy config.example.json config.json >nul
echo Setup complete. Configure config.json and HAS1_BLACKBOX_PASSWORD before start.cmd.
