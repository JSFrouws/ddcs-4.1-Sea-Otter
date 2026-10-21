@echo off
title DDCS V4.1 - connect, step 2 of 2
call "%~dp0files\_bootstrap.bat" || exit /b 1
"%~dp0files\.venv\Scripts\python.exe" "%~dp0files\step2_install.py"
echo.
pause
