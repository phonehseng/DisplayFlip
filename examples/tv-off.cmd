@echo off
rem DisplayFlip hook: put the TV into standby through a Pulse-Eight USB-CEC adapter (libCEC's cec-client).
rem Use it in config.json:  "onTurnOff": ["tv-off.cmd"]   (hooks run from %APPDATA%\DisplayFlip)
set "CEC=%ProgramFiles(x86)%\Pulse-Eight\USB-CEC Adapter\cec-client.exe"
if not exist "%CEC%" set "CEC=%ProgramFiles%\Pulse-Eight\USB-CEC Adapter\cec-client.exe"
if not exist "%CEC%" (echo cec-client.exe not found - install libCEC from pulse-eight.com & exit /b 1)
echo standby 0 | "%CEC%" -s -d 1
