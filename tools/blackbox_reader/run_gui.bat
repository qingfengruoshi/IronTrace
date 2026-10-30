@echo off
chcp 65001 >NUL
cd /d "%~dp0"
set "PY=D:\tmp\pyocd_env\Scripts\python.exe"
if not exist "%PY%" (
  echo [ERROR] Python venv not found: %PY%
  echo Edit run_gui.bat and set PY to your pyOCD Python.
  pause
  exit /b 1
)
echo Starting black-box reader GUI...
"%PY%" blackbox_reader.py --gui
if errorlevel 1 (
  echo.
  echo GUI exited with an error. See message above.
  pause
)
