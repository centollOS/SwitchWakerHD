@echo off
rem Wind Waker HD setup for Windows: double-click this file.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\installer\bootstrap-windows.ps1" %*
if errorlevel 1 pause
