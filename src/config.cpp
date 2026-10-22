#include "config.h"

namespace df {

std::wstring ConfigPath() { return DataDir() + L"\\config.json"; }

const MonitorCfg* Config::Monitor(const LiveMonitor& m) const {
  for (auto& c : monitors)
    if (IEquals(c.match, m.label)) return &c;
  for (auto& c : monitors)
    if (IEquals(c.match, m.name)) return &c;
  for (auto& c : monitors)
    if (!c.match.empty() && IContains(m.key, c.match)) return &c;
  return nullptr;
}

const ProfileCfg* Config::Profile(const std::wstring& name) const {
  for (auto& p : profiles)
    if (IEquals(p.name, name)) return &p;
  return nullptr;
}

bool LoadConfig(const std::wstring& path, Config& out, std::wstring* err) {
  std::string text;
  if (!ReadFileUtf8(path, text)) {
    if (err) *err = L"settings file not found";
    return false;
  }
  std::string perr;
  Json j = Json::Parse(text, &perr);
  if (!perr.empty() || !j.isObj()) {
    if (err) *err = perr.empty() ? L"settings file must be a { ... } object" : Widen(perr);
    return false;
  }
  Config c;
  for (auto& kv : j["hotkeys"].items()) c.hotkeys.emplace_back(Widen(kv.first), kv.second.asWStr());
  c.trayDoubleClick = j.str("trayDoubleClick");
  c.preferredPrimary = j.str("preferredPrimary");
  c.debounceMs = (int)j.num("debounceMs", c.debounceMs);

  const Json& osd = j["osd"];
  c.osd = osd.boolean("enabled", c.osd);
  c.osdMs = (int)osd.num("durationMs", c.osdMs);
  c.osdPosition = osd.str("position", c.osdPosition);

  const Json& w = j["windows"];
  c.remember = w.boolean("remember", c.remember);
  c.orphans = Lower(w.str("whenDisplayTurnsOff", c.orphans));
  c.fixUnplanned = w.boolean("fixAfterSleep", c.fixUnplanned);

  const Json& a = j["audio"];
  c.commsToo = a.boolean("includeCommunications", c.commsToo);
  c.audioFallback = a["fallback"].strList();
  c.audioWaitMs = (int)a.num("waitMs", c.audioWaitMs);

  for (auto& kv : j["monitors"].items()) {
    MonitorCfg m;
    m.match = Widen(kv.first);
    if (kv.second.isStr()) m.audio = kv.second.asWStr();
    else {
      m.audio = kv.second.str("audio");
      m.mic = kv.second.str("mic");
    }
    c.monitors.push_back(m);
  }

  for (auto& kv : j["profiles"].items()) {
    ProfileCfg p;
    p.name = Widen(kv.first);
    const Json& v = kv.second;
    p.hotkey = v.str("hotkey");
    p.audio = v.str("audio");
    p.mic = v.str("mic");
    p.cursor = v.str("cursor");
    p.launch = v["launch"].strList();
    p.closeOnLeave = v["closeOnLeave"].strList();
    p.run = v["run"].strList();
    c.profiles.push_back(p);
  }

  const Json& rules = j["rules"];
  for (size_t i = 0; i < rules.size(); i++) {
    const Json& r = rules.at(i);
    RuleCfg rc;
    rc.app = r.str("app");
    rc.title = r.str("title");
    rc.cls = r.str("class");
    rc.monitor = r.str("monitor");
    rc.state = r.str("state");
    rc.profiles = r["profiles"].strList();
    std::wstring when = Lower(r.str("when", L"both"));
    rc.onOpen = when == L"both" || when == L"open";
    rc.onSwitch = when == L"both" || when == L"switch";
    const Json& rect = r["rect"];
    if (rect.isArr() && rect.size() == 4) {
      rc.hasRect = true;
      LONG x = (LONG)rect.at(0).asNum(), y = (LONG)rect.at(1).asNum();
      rc.rect = RECT{x, y, x + (LONG)rect.at(2).asNum(), y + (LONG)rect.at(3).asNum()};
    }
    if (!rc.monitor.empty()) c.rules.push_back(rc);
  }
  out = c;
  return true;
}

static std::string Q(const std::wstring& s) { return Json(s).Dump(); }
static std::string QCmd(const std::wstring& cmd) { return Json(cmd).Dump(); }

std::string DefaultConfigText(const std::vector<LiveMonitor>& mons, Audio& audio) {
  auto allRender = audio.List(false, false);
  auto hasEndpoint = [&](const std::wstring& name) {
    for (auto& d : allRender)
      if (IEquals(d.desc, name)) return true;
    return false;
  };
  // Pick the "TV": a secondary display that carries its own audio (HDMI first).
  const LiveMonitor* tv = nullptr;
  for (int pass = 0; pass < 2 && !tv; pass++)
    for (auto& m : mons) {
      if (m.primary || !hasEndpoint(m.name)) continue;
      if (pass == 0 && m.tech != (UINT32)DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI) continue;
      tv = &m;
      break;
    }
  // PC sound devices to fall back to: current default first, never a display's own audio
  std::vector<std::wstring> fallback;
  AudioDevice def = audio.Default(false);
  auto isMonitorAudio = [&](const AudioDevice& d) {
    for (auto& m : mons)
      if (IEquals(d.desc, m.name)) return true;
    return false;
  };
  if (!def.name.empty() && !isMonitorAudio(def)) fallback.push_back(def.name);
  for (auto& d : audio.List(false, true)) {
    if (fallback.size() >= 3) break;
    if (isMonitorAudio(d) || d.name == def.name) continue;
    if (IContains(d.name, L"Steam Streaming")) continue;
    fallback.push_back(d.name);
  }

  std::string s;
  s += "// DisplayFlip settings. Changes apply as soon as you save this file.\n";
  s += "//\n";
  s += "// Commands (use them for hotkeys, the tray double-click, Stream Deck or the command line):\n";
  s += "//   monitor \"<display>\" toggle|on|off   turn one display on/off (windows + audio follow)\n";
  s += "//   profile \"<name>\"                    apply a saved layout (tray menu > Save current layout as profile)\n";
  s += "//   toggle \"<A>\" \"<B>\"                  flip between two profiles\n";
  s += "//   next                                cycle through all profiles\n";
  s += "//   primary \"<display>\"                 make a display the main one (games open there)\n";
  s += "//   audio \"<device>\"  /  mic \"<device>\" set the default playback / recording device\n";
  s += "//   restore                             put windows back where they belong right now\n";
  s += "// Displays can be named by their name as shown in the tray menu (\"Name #2\" for identical\n";
  s += "// monitors), or by part of their device id (see the log after running: DisplayFlip.exe diag).\n";
  s += "{\n";
  s += "  \"hotkeys\": {\n";
  if (tv) {
    s += "    \"Ctrl+Alt+T\": " + QCmd(L"monitor " + QuoteArg(tv->label) + L" toggle") + "\n";
  } else {
    s += "    // \"Ctrl+Alt+T\": \"monitor \\\"My TV\\\" toggle\",\n";
    s += "    // \"Ctrl+Alt+R\": \"toggle \\\"Desk\\\" \\\"Rig\\\"\"\n";
  }
  s += "  },\n\n";
  s += "  // What double-clicking the tray icon does\n";
  s += "  \"trayDoubleClick\": " + QCmd(tv ? L"monitor " + QuoteArg(tv->label) + L" toggle" : L"next") + ",\n\n";

  s += "  \"monitors\": {\n";
  s += "    // \"audio\": \"auto\" = sound moves to this display's own HDMI/DisplayPort audio when it turns on,\n";
  s += "    // and goes back to what you used before when it turns off. Or name a device, e.g. an AV receiver.\n";
  if (tv) s += "    " + Q(tv->label) + ": { \"audio\": \"auto\" }\n";
  else s += "    // \"My TV\": { \"audio\": \"auto\" }\n";
  s += "  },\n\n";

  s += "  \"audio\": {\n";
  s += "    // also switch the \"communications\" device (Discord/Teams calls) along with the default one\n";
  s += "    \"includeCommunications\": true,\n";
  s += "    // used when the device you had before is gone (unplugged headset, ...)\n";
  s += "    \"fallback\": [";
  for (size_t i = 0; i < fallback.size(); i++) s += (i ? ", " : "") + Q(fallback[i]);
  s += "]\n";
  s += "  },\n\n";

  s += "  \"windows\": {\n";
  s += "    // remember where every window was for each display setup and put it back\n";
  s += "    \"remember\": true,\n";
  s += "    // windows on a display that turns off: \"minimize\" (they come back when it turns on),\n";
  s += "    // \"move\" (to the main display) or \"windows\" (let Windows decide)\n";
  s += "    \"whenDisplayTurnsOff\": \"minimize\",\n";
  s += "    // also fix windows after a display sleeps/wakes, a cable wiggles or Win+P is used\n";
  s += "    \"fixAfterSleep\": true\n";
  s += "  },\n\n";

  s += "  // small notification shown after each switch\n";
  s += "  \"osd\": { \"enabled\": true, \"durationMs\": 1600, \"position\": \"bottom\" },\n\n";

  s += "  // Extra options for profiles you save from the tray menu (all optional). Example for a sim rig:\n";
  s += "  \"profiles\": {\n";
  s += "    // \"Rig\": {\n";
  s += "    //   \"hotkey\": \"Ctrl+Alt+R\",\n";
  s += "    //   \"audio\": \"Speakers (Focusrite USB Audio)\",   // or \"previous\"\n";
  s += "    //   \"mic\": \"Headset Microphone\",\n";
  s += "    //   \"launch\": [\"C:\\\\Program Files (x86)\\\\SimHub\\\\SimHubWPF.exe\"],  // started if not running\n";
  s += "    //   \"closeOnLeave\": [\"SimHubWPF.exe\"],     // closed when you switch away\n";
  s += "    //   \"cursor\": \"primary\"                    // park the mouse on the main display\n";
  s += "    // }\n";
  s += "  },\n\n";

  s += "  // Put windows on a display automatically, when they open and after every switch.\n";
  s += "  // state: \"normal\" | \"maximized\" | \"minimized\" | \"fill\" | \"fullscreen\";  rect: [x, y, width, height]\n";
  s += "  \"rules\": [\n";
  s += "    // { \"app\": \"SimHubWPF.exe\", \"title\": \"*Dash*\", \"monitor\": \"XENEON EDGE\", \"state\": \"fullscreen\" },\n";
  s += "    // { \"app\": \"Discord.exe\", \"monitor\": \"XENEON EDGE\", \"state\": \"maximized\", \"profiles\": [\"Rig\"] }\n";
  s += "  ]\n";
  s += "}\n";
  return s;
}

bool ParseHotkey(const std::wstring& text, UINT& mods, UINT& vk) {
  mods = 0;
  vk = 0;
  std::wstring t = text;
  std::vector<std::wstring> parts;
  size_t start = 0;
  for (size_t i = 0; i <= t.size(); i++) {
    // allow "Ctrl++" for the plus key
    if (i == t.size() || (t[i] == L'+' && i > start)) {
      parts.push_back(Lower(Trim(t.substr(start, i - start))));
      start = i + 1;
    }
  }
  if (parts.empty()) return false;
  for (size_t i = 0; i + 1 < parts.size(); i++) {
    const std::wstring& p = parts[i];
    if (p == L"ctrl" || p == L"control") mods |= MOD_CONTROL;
    else if (p == L"alt") mods |= MOD_ALT;
    else if (p == L"shift") mods |= MOD_SHIFT;
    else if (p == L"win" || p == L"windows" || p == L"super" || p == L"meta") mods |= MOD_WIN;
    else return false;
  }
  std::wstring k = parts.back();
  if (k.size() == 1 && ((k[0] >= L'a' && k[0] <= L'z') || (k[0] >= L'0' && k[0] <= L'9'))) {
    vk = (UINT)towupper(k[0]);
  } else if (k.size() >= 2 && k[0] == L'f' && iswdigit(k[1])) {
    int n = _wtoi(k.c_str() + 1);
    if (n < 1 || n > 24) return false;
    vk = VK_F1 + n - 1;
  } else if ((IStartsWith(k, L"numpad") || IStartsWith(k, L"num")) && iswdigit(k.back())) {
    vk = VK_NUMPAD0 + (k.back() - L'0');
  } else {
    static const struct { const wchar_t* n; UINT vk; } names[] = {
        {L"left", VK_LEFT}, {L"right", VK_RIGHT}, {L"up", VK_UP}, {L"down", VK_DOWN},
        {L"home", VK_HOME}, {L"end", VK_END}, {L"pageup", VK_PRIOR}, {L"pgup", VK_PRIOR},
        {L"pagedown", VK_NEXT}, {L"pgdn", VK_NEXT}, {L"insert", VK_INSERT}, {L"ins", VK_INSERT},
        {L"delete", VK_DELETE}, {L"del", VK_DELETE}, {L"space", VK_SPACE}, {L"tab", VK_TAB},
        {L"enter", VK_RETURN}, {L"return", VK_RETURN}, {L"esc", VK_ESCAPE}, {L"escape", VK_ESCAPE},
        {L"backspace", VK_BACK}, {L"pause", VK_PAUSE}, {L"break", VK_PAUSE}, {L"scrolllock", VK_SCROLL},
        {L"printscreen", VK_SNAPSHOT}, {L"prtsc", VK_SNAPSHOT}, {L"numpadadd", VK_ADD}, {L"numpadplus", VK_ADD},
        {L"numpadsubtract", VK_SUBTRACT}, {L"numpadminus", VK_SUBTRACT}, {L"numpadmultiply", VK_MULTIPLY},
        {L"numpaddivide", VK_DIVIDE}, {L"numpaddecimal", VK_DECIMAL}, {L"minus", VK_OEM_MINUS},
        {L"-", VK_OEM_MINUS}, {L"plus", VK_OEM_PLUS}, {L"equals", VK_OEM_PLUS}, {L"=", VK_OEM_PLUS},
        {L"+", VK_OEM_PLUS}, {L"comma", VK_OEM_COMMA}, {L",", VK_OEM_COMMA}, {L"period", VK_OEM_PERIOD},
        {L".", VK_OEM_PERIOD}, {L"slash", VK_OEM_2}, {L"/", VK_OEM_2}, {L"backslash", VK_OEM_5},
        {L"\\", VK_OEM_5}, {L"semicolon", VK_OEM_1}, {L";", VK_OEM_1}, {L"quote", VK_OEM_7},
        {L"'", VK_OEM_7}, {L"backtick", VK_OEM_3}, {L"tilde", VK_OEM_3}, {L"`", VK_OEM_3},
        {L"[", VK_OEM_4}, {L"lbracket", VK_OEM_4}, {L"]", VK_OEM_6}, {L"rbracket", VK_OEM_6},
        {L"playpause", VK_MEDIA_PLAY_PAUSE}, {L"mediaplay", VK_MEDIA_PLAY_PAUSE},
        {L"medianext", VK_MEDIA_NEXT_TRACK}, {L"mediaprev", VK_MEDIA_PREV_TRACK}, {L"mediastop", VK_MEDIA_STOP},
        {L"volumeup", VK_VOLUME_UP}, {L"volumedown", VK_VOLUME_DOWN}, {L"volumemute", VK_VOLUME_MUTE},
    };
    for (auto& n : names)
      if (k == n.n) { vk = n.vk; break; }
  }
  return vk != 0;
}

}  // namespace df
