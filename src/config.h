// Settings file (%APPDATA%\DisplayFlip\config.json, JSON with comments).
#pragma once
#include "common.h"
#include "json.h"
#include "display.h"
#include "audio.h"

namespace df {

struct MonitorCfg {
  std::wstring match;   // monitor label / name / part of its device path
  std::wstring audio;   // "auto" (its own HDMI/DP audio), "" / "none", or a device name
  std::wstring mic;
  // commands run when DisplayFlip turns this display on/off (CEC adapter, TV network remote, smart home...)
  std::vector<std::wstring> onTurnOn, onTurnOff;
  int waitMs = 15000;   // how long to wait for a display that only appears after onTurnOn wakes it
  bool HasHooks() const { return !onTurnOn.empty() || !onTurnOff.empty(); }
};

struct ProfileCfg {
  std::wstring name, hotkey, audio, mic, cursor;
  std::vector<std::wstring> launch, closeOnLeave, run;
};

struct RuleCfg {
  std::wstring app, title, cls, monitor, state;
  bool hasRect = false;
  RECT rect{};  // x, y, width, height inside the monitor's work area (stored as l,t,r,b)
  std::vector<std::wstring> profiles;
  bool onOpen = true, onSwitch = true;
};

struct Config {
  std::vector<std::pair<std::wstring, std::wstring>> hotkeys;  // combo -> command
  std::wstring trayDoubleClick;
  std::wstring preferredPrimary;
  // osd
  bool osd = true;
  int osdMs = 1600;
  std::wstring osdPosition = L"bottom";  // bottom | top | center
  // windows
  bool remember = true;
  std::wstring orphans = L"minimize";  // minimize | move | windows
  bool fixUnplanned = true;
  // audio
  bool commsToo = true;
  std::vector<std::wstring> audioFallback;
  int audioWaitMs = 20000;
  int debounceMs = 350;
  std::vector<MonitorCfg> monitors;
  std::vector<ProfileCfg> profiles;
  std::vector<RuleCfg> rules;

  const MonitorCfg* Monitor(const LiveMonitor& m) const;
  const MonitorCfg* MonitorByName(const std::wstring& spec) const;  // for displays that are not connected
  const ProfileCfg* Profile(const std::wstring& name) const;
};

std::wstring ConfigPath();
bool LoadConfig(const std::wstring& path, Config& out, std::wstring* err);
// First run: a commented settings file tailored to the displays/audio found.
std::string DefaultConfigText(const std::vector<LiveMonitor>& mons, Audio& audio);

bool ParseHotkey(const std::wstring& text, UINT& mods, UINT& vk);

}  // namespace df
