@echo off
REM Shared setup for the connecting scripts: find Python and make sure pysmb is available
REM in a local virtual environment under connecting\files\.venv (nothing installed system-wide).
set "FILES=%~dp0"
set "VENV=%FILES%.venv"
set "VPY=%VENV%\Scripts\python.exe"

if exist "%VPY%" goto :havevenv

where py >nul 2>nul
if %errorlevel%==0 (set "PY=py") else (
  where python >nul 2>nul
  if %errorlevel%==0 (set "PY=python") else (
    echo.
    echo   Python is required but was not found.
    echo   Install Python 3 from https://www.python.org/downloads/ ^(tick "Add to PATH"^),
    echo   then run this script again.
    echo.
    pause
    exit /b 1
  )
)

echo   First run: setting up a local Python environment ^(one time^)...
%PY% -m venv "%VENV%"
if not exist "%VPY%" (
  echo   Could not create the Python environment. Is Python 3 installed correctly?
  pause
  exit /b 1
)
"%VPY%" -m pip install --quiet --upgrade pip
"%VPY%" -m pip install --quiet pysmb pycryptodome
echo   Environment ready.

:havevenv
exit /b 0
