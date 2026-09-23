@echo off
rem Starts OpenChat with a startup trace: every step of starting up, and how
rem long it took, printed in this window as it happens. deploy.sh ships this
rem next to OpenChat.exe.
cd /d "%~dp0"
title OpenChat startup trace
echo Starting OpenChat with a startup trace. Keep this window open.
echo If OpenChat is already running, close it first (check the notification
echo area and Task Manager too).
echo.
OpenChat.exe --startup-trace
echo.
echo OpenChat has closed. The trace above is also saved in:
echo   %LOCALAPPDATA%\OpenChat\OpenChat\startup-trace.txt
echo Send that file to whoever gave you this build.
echo.
pause
