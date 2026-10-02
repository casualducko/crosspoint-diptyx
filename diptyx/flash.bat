@echo off
rem Windows launcher: sets up a private Python environment with esptool (nothing is installed system-wide)
rem and runs flash.py. Usage: flash.bat [check|backup|flash] [options]
setlocal
set "HERE=%~dp0"
set "VENV=%HERE%.flasher-venv"
where py >nul 2>nul
if errorlevel 1 goto nopython
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
if errorlevel 1 goto nopython
if exist "%VENV%\.ready" goto run
echo Setting up a private environment (one time, needs internet)...
if exist "%VENV%" rmdir /s /q "%VENV%"
py -3 -m venv "%VENV%" || goto fail
"%VENV%\Scripts\python.exe" -m pip install --quiet --upgrade pip || goto fail
"%VENV%\Scripts\python.exe" -m pip install --quiet --upgrade "esptool>=5" || goto fail
echo.>"%VENV%\.ready"
:run
"%VENV%\Scripts\python.exe" "%HERE%flash.py" %*
set RC=%errorlevel%
if "%~1"=="" pause
exit /b %RC%
:nopython
echo Python 3.10 or newer is required. Install it from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^) and run this again.
if "%~1"=="" pause
exit /b 1
:fail
echo Setting up the environment failed. It needs an internet connection and Python 3.10 or newer.
if exist "%VENV%" rmdir /s /q "%VENV%"
if "%~1"=="" pause
exit /b 1
