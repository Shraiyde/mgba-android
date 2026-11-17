@echo off
title Heart and Soul World Host
cd /d "%~dp0"
if exist HeartSoulWorldHost.exe (
  HeartSoulWorldHost.exe %*
) else if exist bin\Release\net8.0\HeartSoulWorldHost.exe (
  bin\Release\net8.0\HeartSoulWorldHost.exe %*
) else (
  echo HeartSoulWorldHost.exe not found next to this script.
  echo Unzip the release so the EXE and this BAT sit together.
  pause
  exit /b 1
)
pause
