@echo off
title Nest PC Server
rem Double-click: share a folder of videos with Nest on your Vita over Wi-Fi.
cd /d "%~dp0"
set PY=
where py >nul 2>nul && set PY=py -3
if not defined PY where python >nul 2>nul && python -c "import sys; sys.exit(0 if sys.version_info>=(3,8) else 1)" >nul 2>nul && set PY=python
for %%V in (314 313 312 311 310 39 38) do if not defined PY if exist "%LOCALAPPDATA%\Programs\Python\Python%%V\python.exe" set PY="%LOCALAPPDATA%\Programs\Python\Python%%V\python.exe"
if not defined PY (
  echo.
  echo  Python 3 isn't installed on this PC yet.
  echo  Get it free from https://www.python.org/downloads/  ^(tick "Add python.exe to PATH"^),
  echo  then double-click me again.
  echo.
  pause
  exit /b 1
)
%PY% "%~dp0nest_pc_server.py"
if errorlevel 1 pause
