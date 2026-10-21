@echo off
title DDCS V4.1 - restore the original application
call "%~dp0files\_bootstrap.bat" || exit /b 1
"%~dp0files\.venv\Scripts\python.exe" "%~dp0files\step3_reset.py"
echo.
pause
