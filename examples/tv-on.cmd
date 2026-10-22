@echo off
rem DisplayFlip hook: wake the TV and switch it to this PC's HDMI input (Pulse-Eight USB-CEC adapter).
rem Use it in config.json:  "onTurnOn": ["tv-on.cmd"]   (hooks run from %APPDATA%\DisplayFlip)
set "CEC=%ProgramFiles(x86)%\Pulse-Eight\USB-CEC Adapter\cec-client.exe"
if not exist "%CEC%" set "CEC=%ProgramFiles%\Pulse-Eight\USB-CEC Adapter\cec-client.exe"
if not exist "%CEC%" (echo cec-client.exe not found - install libCEC from pulse-eight.com & exit /b 1)
echo on 0 | "%CEC%" -s -d 1
echo as | "%CEC%" -s -d 1
