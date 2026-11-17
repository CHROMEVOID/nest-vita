@echo off
title Nest Metadata
rem Finds posters, logos and descriptions for your videos and saves them next to the files. Double-click me, or drag a video folder onto me.
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
%PY% "%~dp0nest_meta.py" %*
echo.
echo Done. Copy the folder to your Vita (or rescan it in Nest) to see the new artwork.
pause
