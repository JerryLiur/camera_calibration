@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0spike_calibration_tool\start.ps1"
if errorlevel 1 pause
