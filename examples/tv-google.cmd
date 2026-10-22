@echo off
rem DisplayFlip hook: turn a Google Home device (e.g. your Google TV) on/off through Google. See tv-google.py.
rem First time only: sets up a private Python environment next to this file.
setlocal
set "PYENV=%~dp0py"
if not exist "%PYENV%\Scripts\python.exe" (
  echo Setting up Python packages for tv-google, one time...
  python -m venv "%PYENV%" || (echo Python 3.11+ is needed: winget install Python.Python.3.12 & exit /b 1)
  "%PYENV%\Scripts\python.exe" -m pip install --quiet --disable-pip-version-check gassist-text google-auth-oauthlib || exit /b 1
)
"%PYENV%\Scripts\python.exe" "%~dp0tv-google.py" %*
