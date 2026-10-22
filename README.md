# DisplayFlip

One button to turn your TV (or any display) on/off, or switch between whole display
setups (desk, couch, sim rig), with **windows and sound following automatically**.
No dialogs, no "keep these display settings?", no waiting on a settings app.

- **Native and tiny**: one ~770 KB exe, ~2 MB of private RAM, no runtime or installer needed.
- **Fast**: a TV on/off takes 0.5–0.9 s (that is Windows' own mode switch; DisplayFlip adds ~20 ms).
- **Remembers windows per display setup.** Turn the TV off and back on and everything that
  was on it returns, maximized or not. The first time in a session nothing is moved onto the TV.
  Windows on the other screens stay exactly where they were.
- **Sound follows the display.** Turning the TV on switches the default playback device to the TV's HDMI
  audio the moment Windows exposes it. Turning it off switches back to whatever you used before
  (headphones/speakers), *before* the TV drops, so there is no gap or random device in between.
- **Fixes the classic multi-monitor mess**: when a display sleeps/wakes, a cable wiggles or you press
  Win+P, windows go back to where they were.
- **Stream Deck plugin** with live state (the button lights up when the TV/profile is on),
  global hotkeys, tray menu, and a command line for anything else (macros, AutoHotkey, scripts).
- **Sim racing friendly**: profiles that switch the main display (so games open on the rig's center
  screen), set refresh rates/resolutions, launch SimHub/CrewChief, close them when you leave, and
  window rules that pin dashboards or Discord to a specific screen.

## Install

```powershell
DisplayFlip.exe install
```

Copies itself to `%LOCALAPPDATA%\Programs\DisplayFlip`, starts with Windows, adds **DisplayFlip** to the
Start menu, installs the Stream Deck plugin (Stream Deck restarts once), and starts in the tray. No admin
rights needed. Opening DisplayFlip from the Start menu while it is already running pops up its tray menu.
`DisplayFlip.exe uninstall` reverses it; settings stay in `%APPDATA%\DisplayFlip`.

To build it yourself: install [MSYS2](https://www.msys2.org/) with `mingw-w64-ucrt-x86_64-gcc`, then run
`powershell -ExecutionPolicy Bypass -File build.ps1 -Install`.

## Using it

On first start DisplayFlip writes a commented settings file tailored to your hardware. If it finds a
secondary display with its own audio (a TV), it sets up:

| | |
|---|---|
| **Ctrl+Alt+T** | turn that display on/off |
| **Double-click tray icon** | the same |
| **Right-click tray icon** | profiles, every display (click to turn it on/off), main display, settings |

**Profiles**: arrange your displays however you like (Windows Settings, NVIDIA panel, anything),
then tray menu > *Save current layout as profile…*. A profile stores which displays are on, their
positions, resolutions, refresh rates, rotation and which one is the main display. Monitors are
identified by their EDID, so profiles keep working after reboots, driver updates, or moving
a cable to another port.

### Stream Deck

Find **DisplayFlip** in the action list:

- **Toggle Display**: pick a display (default: the TV). Lit while it is on.
- **Apply Profile**: lit while that profile is active.
- **Toggle Profiles**: flip between two profiles (e.g. Desk / Rig).
- **Run Command**: any command below.

A plain Stream Deck *Hotkey* action sending Ctrl+Alt+T works too.

### Commands

The same commands work for hotkeys, the tray double-click, Stream Deck and the command line
(`DisplayFlip.exe <command>`; it hands the command to the running app instantly):

```
monitor "Philips GA4" toggle     turn a display on/off (also: on, off)
tv [toggle|on|off]               same, for the first display listed in settings
profile "Rig"                    apply a profile
toggle "Desk" "Rig"              flip between two profiles
next                             cycle through profiles
primary "LG ULTRAGEAR+"          make a display the main one
save "Rig"                       save the current layout as a profile
audio "Headphones"               set the default playback device (mic "..." for recording)
restore                          put windows back after the last switch
hook "Philips GA4" off|on        run a display's onTurnOff/onTurnOn commands (testing)
status | list | diag | windows   show state (diag also goes to the log)
settings | log | reload | quit
```

Displays are named as in the tray menu. Identical monitors (triple screens) get `Name #1`,
`Name #2`, `Name #3`; you can also use part of a monitor's device id from `diag`.

## Settings

`%APPDATA%\DisplayFlip\config.json` (JSON with comments; saving the file applies it immediately).

```jsonc
{
  "hotkeys": {
    "Ctrl+Alt+T": "monitor \"Philips GA4\" toggle",
    "Ctrl+Alt+R": "toggle \"Desk\" \"Rig\""
  },
  "trayDoubleClick": "monitor \"Philips GA4\" toggle",
  "preferredPrimary": "LG ULTRAGEAR+",       // who becomes main if the main display is turned off

  "monitors": {
    "Philips GA4": { "audio": "auto",        // sound follows this display (its own HDMI audio)
                     "onTurnOff": ["tv-off.cmd"], "onTurnOn": ["tv-on.cmd"] },   // see "Turning the TV itself off"
    "Projector":   { "audio": "Denon AVR" }  // ...or a specific device, e.g. an AV receiver
  },

  "audio": {
    "includeCommunications": true,           // also move the Discord/Teams "communications" device
    "fallback": ["Headphones", "Speakers"],  // if the device you had before is gone
    "waitMs": 20000                          // how long to wait for a TV's audio to appear
  },

  "windows": {
    "remember": true,
    "whenDisplayTurnsOff": "minimize",       // or "move" (to the main display) or "windows"
    "fixAfterSleep": true
  },

  "osd": { "enabled": true, "durationMs": 1600, "position": "bottom" },   // bottom | top | center
  "debounceMs": 350,                         // ignore a second press that comes in during a switch

  "profiles": {
    "Rig": {
      "hotkey": "Ctrl+Alt+R",
      "audio": "Speakers (Focusrite USB Audio)",   // or "previous"
      "mic": "Headset Microphone",
      "launch": ["C:\\Program Files (x86)\\SimHub\\SimHubWPF.exe"],   // started if not running
      "closeOnLeave": ["SimHubWPF.exe"],            // asked to close when you switch away
      "run": ["powershell -File C:\\scripts\\rig-on.ps1"],
      "cursor": "primary"                           // park the mouse on the main display
    },
    "Desk": { "audio": "previous" }
  },

  "rules": [
    // placed when they open and after every switch; first match wins
    { "app": "SimHubWPF.exe", "title": "*Dash*", "monitor": "XENEON EDGE", "state": "fullscreen" },
    { "app": "Discord.exe", "monitor": "XENEON EDGE", "state": "maximized", "profiles": ["Rig"] },
    { "app": "crewchief*.exe", "monitor": "Side #2", "rect": [0, 0, 900, 700] }
  ]
}
```

Hotkey keys: `A`–`Z`, `0`–`9`, `F1`–`F24`, `Numpad0`–`Numpad9`, arrows, `Home`, `End`, `PageUp`, `PageDown`,
`Insert`, `Delete`, `Space`, `Pause`, `ScrollLock`, media keys, punctuation, with `Ctrl`, `Alt`, `Shift`, `Win`.
F13–F24 make good Stream Deck/macro-key combos because no keyboard uses them.

## Turning the TV itself off and on

When DisplayFlip turns a display off, Windows just stops sending it a picture; PC graphics cards can't send
the HDMI-CEC "standby" or "power on" signals a console would. So without help, a TV that is in standby stays
dark when you switch to it (DisplayFlip tells you so instead of failing silently). Give the display `onTurnOff` / `onTurnOn` commands and
DisplayFlip runs them at the right moment: `onTurnOn` right before the output comes back (so the TV wakes in
parallel), `onTurnOff` once nothing is shown on it any more.

```jsonc
"monitors": {
  "Philips GA4": {
    "audio": "auto",
    "onTurnOff": ["tv-off.cmd"],
    "onTurnOn": ["tv-on.cmd"],
    "waitForDisplayMs": 15000   // a TV that vanishes in standby: how long to wait for it after waking it
  }
}
```

- Commands run hidden, one after another, from `%APPDATA%\DisplayFlip`, with `%DISPLAYFLIP_DISPLAY%` and
  `%DISPLAYFLIP_EVENT%` (`on`/`off`) set. Their output goes to `hooks.log` in that folder.
- `DisplayFlip.exe hook "Philips GA4" off` runs the commands without switching anything, for testing.
- Some TVs disappear from Windows completely in standby. DisplayFlip then shows them in the tray as
  *(asleep)*: pressing the button runs `onTurnOn`, waits for the TV to show up, then turns it on.
  Pressing again while it's waking cancels and sends it back to standby.

Ready-made options:

| | |
|---|---|
| **USB-CEC adapter** (Pulse-Eight, sits in the HDMI cable) | [`examples/tv-off.cmd`](examples/tv-off.cmd) / [`tv-on.cmd`](examples/tv-on.cmd) use libCEC's `cec-client` (standby / wake + switch input to the PC) |
| **Google / Android TV over the network** | `adb connect 192.168.1.50 && adb -s 192.168.1.50:5555 shell input keyevent KEYCODE_SLEEP` (`KEYCODE_WAKEUP` to wake; needs network debugging on the TV) |
| **Home Assistant / smart home** | `curl -s -X POST http://homeassistant.local:8123/api/webhook/tv_off` |

Avoid cutting a TV's power with a smart plug: OLEDs run a screen-care cycle in standby after you turn them off.

## Sim rig recipe

1. Set the rig up (triple screens, the center one as main display, the refresh rates you race at) and
   save it as **Rig**. Set your desk up and save it as **Desk**.
2. Add `"Ctrl+Alt+R": "toggle \"Desk\" \"Rig\""` (or a Stream Deck *Toggle Profiles* button).
3. Optional: in `profiles.Rig` set the audio device for the wheel/headset, `launch` SimHub,
   and add `rules` to pin the SimHub dash or Discord to the side/dash screen.

Games open on the main display, so switching the main display with the profile is what makes a
game launch on the rig. NVIDIA Surround / AMD Eyefinity spans are driver features that Windows'
display API cannot toggle; with Surround on, the span shows up as one big display (that works
in profiles, but enabling/disabling Surround itself still happens in the NVIDIA app).

## How it works

- **Displays**: the Windows CCD API (`QueryDisplayConfig`/`SetDisplayConfig`), the same one Windows
  Settings uses. Layouts are applied exactly (modes, positions, timings) in a single call. If Windows
  rejects that (for example a monitor's modes changed), DisplayFlip falls back step by step to letting
  Windows fill in details from its own display database. It never shows the "keep changes?" prompt.
- **Windows**: every few seconds (and right before every switch) it notes where each app window is,
  per display setup. After a switch it puts windows back in several quick passes over ~2.5 s,
  because Windows and some apps keep nudging windows for a moment after a display change. If you
  grab a window during that time it stops touching that window.
- **Audio**: Core Audio for device discovery and change notifications, plus the `IPolicyConfig` interface
  the Sound control panel itself uses to set defaults. A TV's HDMI audio endpoint only exists while the TV is
  active, so DisplayFlip waits for Windows to announce it (typically 0.5–1 s) instead of polling blindly.
- **Self-healing**: an internal crash guard logs the problem and restarts the app so the hotkey keeps working.

## Troubleshooting

- **Hotkey doesn't work**: another app owns that combo; the OSD/log says so. Pick another in settings.
- **An admin window (Task Manager, elevated terminal) doesn't move back**: Windows blocks normal apps
  from moving elevated windows. Run DisplayFlip elevated if you need that.
- **TV doesn't come on**: a TV in standby ignores the PC's picture, and a PC can't power a TV on over plain
  HDMI. DisplayFlip notices within about a second and says "didn't turn on"; turn the TV on with its remote and
  press again, or give it `onTurnOn` commands (see above). With those, DisplayFlip waits a few seconds for the TV
  to boot and tries once more. `DisplayFlip.exe diag` lists what Windows can see.
- **Other display managers**: DisplayFusion's window-position features, DisplayMagician and similar tools can
  fight over the same windows/displays. Turn those features off (or quit those apps) if windows jump twice.
- Log: tray > *Open log* (`%APPDATA%\DisplayFlip\displayflip.log`, includes timing for every switch).
