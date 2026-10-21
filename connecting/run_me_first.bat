@echo off
title DDCS V4.1 - connect, step 1 of 2
call "%~dp0files\_bootstrap.bat" || exit /b 1
"%~dp0files\.venv\Scripts\python.exe" "%~dp0files\step1_swap.py"
echo.
pause
