@echo off
rem Wind Waker HD setup in a console window (Windows): the fallback for "Wind Waker HD.exe".
setlocal
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\bootstrap-windows.ps1" %*
if errorlevel 1 pause
