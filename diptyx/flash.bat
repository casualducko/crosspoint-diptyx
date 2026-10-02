@echo off
rem Windows launcher: sets up a private Python environment with esptool (nothing is installed system-wide)
rem and runs flash.py. Usage: flash.bat [check|backup|flash] [options]
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  echo Python 3 is required. Install it from https://www.python.org/downloads/ and tick "Add python.exe to PATH".
  exit /b 1
)
if not exist .flasher-venv\Scripts\python.exe (
  echo Setting up a private environment ^(one time, needs internet^)...
  py -3 -m venv .flasher-venv || exit /b 1
  .flasher-venv\Scripts\python.exe -m pip install --quiet --upgrade pip
  .flasher-venv\Scripts\python.exe -m pip install --quiet --upgrade "esptool>=5" || exit /b 1
)
.flasher-venv\Scripts\python.exe flash.py %*
