@echo off
rem DisplayFlip hook: wake the TV through a Pulse-Eight USB-CEC adapter and switch it to the PC's input.
rem Use it in config.json:  "onTurnOn": ["tv-on.cmd"]   (hooks run from %APPDATA%\DisplayFlip)
rem
rem Works with the adapter in-line or on a spare TV input next to the PC's own cable (the way to keep
rem 4K 120/144 Hz, which the adapter can't pass through). PC_INPUT is the TV input the PC's video cable
rem uses: the TV then switches there instead of to the adapter's input.
set "PC_INPUT=3"
set "CEC=%ProgramFiles(x86)%\Pulse-Eight\USB-CEC Adapter\cec-client.exe"
if not exist "%CEC%" set "CEC=%ProgramFiles%\Pulse-Eight\USB-CEC Adapter\cec-client.exe"
if not exist "%CEC%" (echo cec-client.exe not found - install libCEC from pulse-eight.com & exit /b 1)
echo on 0 | "%CEC%" -s -d 1 -t p
rem CEC "Active Source" for physical address <PC_INPUT>.0.0.0, sent as playback device 1
echo tx 4F:82:%PC_INPUT%0:00 | "%CEC%" -s -d 1 -t p
