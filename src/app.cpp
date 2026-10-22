#include "app.h"
#include "audio.h"
#include "config.h"
#include "display.h"
#include "osd.h"
#include "resource.h"
#include "winlayout.h"
#include <shellapi.h>
#include <shellscalingapi.h>
#include <cmath>

namespace df {

const wchar_t* kMainClass = L"DisplayFlip.Main";

namespace {

enum : UINT { WM_APP_TRAY = WM_APP + 1, WM_APP_AUDIO = WM_APP + 2 };
enum : UINT_PTR { T_PASS = 11, T_AUDIO, T_DISPCHANGE, T_SNAPSHOT, T_CONFIG, T_RULES, T_SAVE, T_WAKE };
// Restore passes after a switch: Windows and some apps keep nudging windows for a
// second or two after the topology changes, so we re-check a few times.
const int kPassDelays[] = {100, 250, 500, 900, 1600};
const int kHotkeyBase = 100;

enum : UINT {
  ID_SAVE_NEW = 100, ID_SETTINGS, ID_LOG, ID_AUTOSTART, ID_RELOAD, ID_EXIT, ID_RESTORE, ID_FOLDER, ID_STREAMDECK,
  ID_PROFILE = 1000, ID_MONITOR = 2000, ID_UPDATE = 3000, ID_DELETE = 4000, ID_PRIMARY = 5000, ID_WAKE = 6000
};

bool IsNone(const std::wstring& s) { return s.empty() || IEquals(s, L"none") || IEquals(s, L"off"); }

std::wstring TechName(UINT32 t) {
  switch ((DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY)t) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"eDP";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return L"DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return L"VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"internal";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return L"Miracast";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return L"USB/indirect";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL: return L"virtual";
    default: return Format(L"type %u", t);
  }
}

std::wstring Fingerprint() {
  return Format(L"%d|%d,%d,%d,%d", GetSystemMetrics(SM_CMONITORS), GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

// ---- tiny text prompt (used for "Save current layout as profile") ----------
struct PromptCtx {
  std::wstring* value;
  std::wstring label;
  bool ok = false, done = false;
  HWND edit = nullptr;
  HFONT font = nullptr;
};

LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  PromptCtx* c = (PromptCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
  switch (m) {
    case WM_CREATE: {
      c = (PromptCtx*)((CREATESTRUCTW*)l)->lpCreateParams;
      SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
      UINT dpi = GetDpiForWindow(h);
      auto S = [&](int v) { return MulDiv(v, dpi, 96); };
      c->font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
      HWND st = CreateWindowExW(0, L"STATIC", c->label.c_str(), WS_CHILD | WS_VISIBLE, S(14), S(12), S(332), S(20), h,
                                nullptr, nullptr, nullptr);
      c->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", c->value->c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                S(14), S(36), S(332), S(26), h, (HMENU)100, nullptr, nullptr);
      HWND ok = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(176), S(74),
                                S(82), S(28), h, (HMENU)IDOK, nullptr, nullptr);
      HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(264), S(74), S(82), S(28), h,
                                    (HMENU)IDCANCEL, nullptr, nullptr);
      for (HWND x : {st, c->edit, ok, cancel}) SendMessageW(x, WM_SETFONT, (WPARAM)c->font, TRUE);
      SendMessageW(c->edit, EM_SETSEL, 0, -1);
      SetFocus(c->edit);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(w) == IDOK) {
        wchar_t buf[256] = {};
        GetWindowTextW(c->edit, buf, 255);
        *c->value = Trim(buf);
        c->ok = !c->value->empty();
        DestroyWindow(h);
      } else if (LOWORD(w) == IDCANCEL) {
        DestroyWindow(h);
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (c) {
        c->done = true;
        DeleteObject(c->font);
      }
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

bool PromptText(HINSTANCE inst, const std::wstring& title, const std::wstring& label, std::wstring& value) {
  static bool reg = false;
  if (!reg) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PromptProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"DisplayFlip.Prompt";
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    reg = true;
  }
  PromptCtx c;
  c.value = &value;
  c.label = label;
  POINT o{0, 0};
  HMONITOR hm = MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(hm, &mi);
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
  RECT r{0, 0, MulDiv(360, dx, 96), MulDiv(114, dx, 96)};
  AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dx);
  int w = r.right - r.left, h = r.bottom - r.top;
  int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
  HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"DisplayFlip.Prompt", title.c_str(),
                             WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, nullptr, nullptr, inst, &c);
  SetForegroundWindow(dlg);
  MSG msg;
  while (!c.done && GetMessageW(&msg, nullptr, 0, 0)) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { DestroyWindow(dlg); continue; }
    if (!IsDialogMessageW(dlg, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return c.ok;
}

// ---- per-display hooks (CEC adapter, TV network remote, smart home...) -----
struct HookJob {
  std::vector<std::wstring> cmds;
  std::wstring display, event;
};

// Runs a display's commands one after another on a worker thread (so a slow
// cec-client never holds up a switch). Output goes to hooks.log.
DWORD WINAPI HookThread(LPVOID p) {
  std::unique_ptr<HookJob> job((HookJob*)p);
  std::wstring dir = DataDir();
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = CreateFileW((dir + L"\\hooks.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  // environment for the commands: ours plus DISPLAYFLIP_DISPLAY / DISPLAYFLIP_EVENT
  std::wstring env;
  if (wchar_t* e = GetEnvironmentStringsW()) {
    for (wchar_t* v = e; *v; v += wcslen(v) + 1) env.append(v).push_back(L'\0');
    FreeEnvironmentStringsW(e);
  }
  env.append(L"DISPLAYFLIP_DISPLAY=" + job->display).push_back(L'\0');
  env.append(L"DISPLAYFLIP_EVENT=" + job->event).push_back(L'\0');
  env.push_back(L'\0');
  for (auto& c : job->cmds) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string head = Narrow(Format(L"\r\n[%02d:%02d:%02d] %ls turned %ls: %ls\r\n", st.wHour, st.wMinute, st.wSecond,
                                     job->display.c_str(), job->event.c_str(), c.c_str()));
    DWORD w;
    if (out != INVALID_HANDLE_VALUE) WriteFile(out, head.data(), (DWORD)head.size(), &w, nullptr);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi = {};
    std::wstring cl = L"cmd.exe /d /s /c \"" + c + L"\"";
    ULONGLONG t0 = NowMs();
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        (LPVOID)env.data(), dir.c_str(), &si, &pi)) {
      Log(L"hook (%ls %ls) could not start: %ls (error %lu)", job->display.c_str(), job->event.c_str(), c.c_str(),
          GetLastError());
      continue;
    }
    DWORD code = 0;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_TIMEOUT) Log(L"hook still running after 30 s: %ls", c.c_str());
    GetExitCodeProcess(pi.hProcess, &code);
    Log(L"hook (%ls %ls) exit %lu after %llu ms: %ls", job->display.c_str(), job->event.c_str(), code, NowMs() - t0,
        c.c_str());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
  if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
  return 0;
}

void RunHooks(const std::vector<std::wstring>& cmds, const std::wstring& display, const wchar_t* event) {
  if (cmds.empty()) return;
  HANDLE t = CreateThread(nullptr, 0, HookThread, new HookJob{cmds, display, event}, 0, nullptr);
  if (t) CloseHandle(t);
}

struct SwitchSpec {
  std::wstring title;
  std::function<bool(std::wstring*)> apply;  // display change, may be empty
  bool expectTopologyChange = false;
  std::wstring audioBefore, audioAfter, micAfter;
  std::wstring profile, leavingProfile;
  std::vector<HookJob> hooksBefore, hooksAfter;  // "on" hooks wake a TV first, "off" hooks run once it is dark
  std::vector<std::wstring> expectOn;  // keys of displays this switch must light up
  std::wstring replay;  // try the command once more after a TV we just woke (CEC/network) had time to start
};

// A display that is so deeply asleep Windows can't see it: its onTurnOn hook was
// run and we wait for it to show up, then replay the command.
struct PendingWake {
  bool active = false;
  std::vector<std::wstring> names;
  std::wstring command;
  bool applyAnyway = false;  // profiles go ahead without it if it never shows up
  ULONGLONG since = 0, deadline = 0;
  ULONGLONG notBefore = 0;  // retries: give the TV a few seconds to boot first
};

struct ProfileData {
  std::wstring name;
  DisplaySnap snap;
  bool hasSnap = false;
};

struct PendingAudio {
  bool active = false;
  std::wstring spec;
  ULONGLONG since = 0, deadline = 0;
};

class App;
App* g_app = nullptr;

void CALLBACK WinEventThunk(HWINEVENTHOOK, DWORD ev, HWND h, LONG idObj, LONG idChild, DWORD, DWORD);

class App {
 public:
  int Run(HINSTANCE inst, const std::wstring& first);
  LRESULT Proc(HWND h, UINT m, WPARAM w, LPARAM l);
  void OnWinEvent(DWORD ev, HWND h, LONG idObj, LONG idChild);

 private:
  // commands
  std::string Exec(const std::wstring& line, HWND replyTo);
  std::string DoMonitor(const std::wstring& spec, const std::wstring& action);
  std::string DoProfile(const std::wstring& name);
  std::string DoToggle(const std::wstring& a, const std::wstring& b);
  std::string DoNext();
  std::string DoPrimary(const std::wstring& spec);
  std::string DoAudio(const std::wstring& spec, bool capture);
  std::string SaveProfile(std::wstring name, bool prompt);
  std::string DeleteProfile(const std::wstring& name);
  std::string RestoreNow();
  bool DoSwitch(const SwitchSpec& s);
  void StartWake(const std::vector<std::wstring>& names, const std::wstring& command, bool applyAnyway);
  void StartRetry(const std::vector<std::wstring>& names, const std::wstring& command);
  void CancelWake(bool runOffHooks);
  void OnWakeTimer();
  bool RecentlyWoken(const std::wstring& name);
  std::vector<std::wstring> AsleepDisplays();  // configured with onTurnOn but not visible to Windows

  // audio
  bool ResolveAudio(const std::wstring& spec, bool capture, AudioDevice& out);
  bool ApplyAudio(const std::wstring& spec, bool capture, std::wstring* msg);
  void RequestAudio(const std::wstring& spec, bool capture);
  void TryPendingAudio();
  bool AudioIsOn(const MonitorCfg& mc, const LiveMonitor& m);

  // state
  void RefreshMonitorMemory();
  void LoadProfiles();
  std::vector<std::wstring> ProfileNames();
  std::wstring ResolveProfile(const std::wstring& name);
  std::wstring DetectProfile();
  std::wstring DefaultMonitorSpec();
  void ReloadConfig(bool initial);
  void RegisterHotkeys();
  void LoadState();
  void SaveState();
  void MarkDirty() { SetTimer(wnd_, T_SAVE, 2500, nullptr); }
  void OnDisplayChange();
  void HandleUnplanned();
  void Snapshot();
  void RunPass();
  std::set<HWND> ApplyRules(HWND only, bool onOpen);
  void UpdateRuleHook();
  void ProcessPendingRules();
  void ProfileExtras(const std::wstring& name);
  void CloseApps(const std::vector<std::wstring>& exes);
  std::string StatusJson(bool withAudio);
  std::string Diag();
  void Broadcast();

  // tray
  void AddTray();
  void UpdateTray();
  void ShowMenu();
  void OnMenu(UINT id);

  HINSTANCE inst_ = nullptr;
  HWND wnd_ = nullptr;
  UINT taskbarCreated_ = 0;
  Config cfg_;
  ULONGLONG cfgTime_ = 0;
  Audio audio_;
  Osd osd_;
  std::vector<LiveMonitor> mons_;
  std::wstring topo_, fingerprint_;
  std::map<std::wstring, Layout> memory_;     // topology -> window layout last seen there
  std::map<std::wstring, PathSnap> monMem_;   // monitor -> last placement while it was on
  std::map<std::wstring, ProfileData> profiles_;  // lower-case name -> profile
  std::wstring activeProfile_, lastProfile_;
  std::wstring audioPrevId_, micPrevId_, audioOnMonitor_;
  PendingAudio pending_[2];
  std::vector<WinTarget> plan_, lastPlan_;
  int passIdx_ = 0;
  HWND restoreFg_ = nullptr;
  bool switching_ = false;
  ULONGLONG lastSwitchEnd_ = 0, suppressUntil_ = 0, freezeSnapshotUntil_ = 0;
  DWORD switchStart32_ = 0, switchEnd32_ = 0;
  std::vector<std::wstring> hotkeyCmds_;
  std::vector<std::wstring> hotkeyErrors_;
  std::vector<HWND> subs_;
  HWINEVENTHOOK hookShow_ = nullptr, hookMove_ = nullptr;
  std::set<HWND> ruleSeen_;
  std::vector<std::pair<HWND, ULONGLONG>> rulePending_;
  std::vector<std::wstring> menuProfiles_;
  std::vector<LiveMonitor> menuMons_;
  std::vector<std::wstring> menuAsleep_;
  PendingWake wake_;
  std::map<std::wstring, ULONGLONG> recentWake_;  // lower-case name -> when its onTurnOn hook last ran
  bool trayAdded_ = false;
};

LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (g_app) return g_app->Proc(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}

void CALLBACK WinEventThunk(HWINEVENTHOOK, DWORD ev, HWND h, LONG idObj, LONG idChild, DWORD, DWORD) {
  if (g_app) g_app->OnWinEvent(ev, h, idObj, idChild);
}

// ---- lifecycle -------------------------------------------------------------
int App::Run(HINSTANCE inst, const std::wstring& first) {
  inst_ = inst;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASSW wc = {};
  wc.lpfnWndProc = StaticProc;
  wc.hInstance = inst;
  wc.lpszClassName = kMainClass;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
  RegisterClassW(&wc);
  wnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"DisplayFlip", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
  // allow commands from non-elevated callers (Stream Deck, shortcuts) even if we run elevated
  ChangeWindowMessageFilterEx(wnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  ChangeWindowMessageFilterEx(wnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);

  osd_.Init(inst);
  audio_.Init(wnd_, WM_APP_AUDIO);
  LoadState();
  mons_ = QueryMonitors();
  topo_ = TopologyKey(mons_);
  fingerprint_ = Fingerprint();
  RefreshMonitorMemory();
  bool firstRun = !FileExists(ConfigPath());
  if (firstRun) {
    WriteFileUtf8(ConfigPath(), DefaultConfigText(mons_, audio_));
    Log(L"wrote default settings to %ls", ConfigPath().c_str());
  }
  ReloadConfig(true);
  if (cfg_.remember) memory_[topo_] = CaptureLayout(MonMap::Current(mons_));
  activeProfile_ = DetectProfile();
  AddTray();
  hookMove_ = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZESTART, nullptr, WinEventThunk, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  UpdateRuleHook();
  SetTimer(wnd_, T_SNAPSHOT, 4000, nullptr);
  SetTimer(wnd_, T_CONFIG, 1500, nullptr);
  int on = 0;
  for (auto& m : mons_) on += m.active;
  Log(L"DisplayFlip %ls started: %d of %u displays on, profile '%ls'", DF_VERSION, on, (unsigned)mons_.size(),
      activeProfile_.c_str());

  if (firstRun && cfg_.osd) {
    std::wstring hint = L"Right-click the tray icon for profiles and settings";
    if (!cfg_.hotkeys.empty()) hint = cfg_.hotkeys[0].first + L": " + cfg_.hotkeys[0].second;
    osd_.Show(L"DisplayFlip is ready", hint, OsdKind::Ok, 4000, cfg_.osdPosition);
  } else if (!hotkeyErrors_.empty() && cfg_.osd) {
    osd_.Show(L"Hotkey problem", hotkeyErrors_[0], OsdKind::Warn, 4000, cfg_.osdPosition);
  }
  if (!first.empty()) Exec(first, nullptr);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  SaveState();
  if (trayAdded_) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = wnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
  }
  if (hookShow_) UnhookWinEvent(hookShow_);
  if (hookMove_) UnhookWinEvent(hookMove_);
  audio_.Shutdown();
  CoUninitialize();
  Log(L"DisplayFlip exited");
  return 0;
}

LRESULT App::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_HOTKEY: {
      size_t idx = (size_t)w - kHotkeyBase;
      if (idx >= hotkeyCmds_.size()) return 0;
      DWORD t = (DWORD)GetMessageTime();
      // presses that happened while a switch was running (or right after) are dropped
      if (switchEnd32_ && (LONG)(t - switchStart32_) >= 0 && (LONG)(t - switchEnd32_) < cfg_.debounceMs) {
        Log(L"hotkey ignored (pressed during a switch)");
        return 0;
      }
      Exec(hotkeyCmds_[idx], nullptr);
      return 0;
    }
    case WM_COPYDATA: {
      auto* cds = (COPYDATASTRUCT*)l;
      if (cds->dwData != kCdCommand) return FALSE;
      std::string s((const char*)cds->lpData, cds->cbData);
      Exec(Widen(s), (HWND)w);
      return TRUE;
    }
    case WM_DISPLAYCHANGE:
      OnDisplayChange();
      break;
    case WM_POWERBROADCAST:
      if (w == PBT_APMSUSPEND) freezeSnapshotUntil_ = NowMs() + 24ULL * 3600 * 1000;
      if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) freezeSnapshotUntil_ = NowMs() + 8000;
      return TRUE;
    case WM_TIMER:
      switch (w) {
        case T_PASS: RunPass(); break;
        case T_AUDIO: TryPendingAudio(); break;
        case T_DISPCHANGE: KillTimer(h, T_DISPCHANGE); HandleUnplanned(); break;
        case T_SNAPSHOT: Snapshot(); break;
        case T_CONFIG:
          if (FileWriteTime(ConfigPath()) != cfgTime_) ReloadConfig(false);
          break;
        case T_RULES: ProcessPendingRules(); break;
        case T_SAVE: KillTimer(h, T_SAVE); SaveState(); break;
        case T_WAKE: OnWakeTimer(); break;
      }
      return 0;
    case WM_APP_AUDIO:
      if (pending_[0].active || pending_[1].active) TryPendingAudio();
      return 0;
    case WM_APP_TRAY:
      switch (LOWORD(l)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: ShowMenu(); break;
        case WM_LBUTTONDBLCLK: Exec(cfg_.trayDoubleClick.empty() ? L"next" : cfg_.trayDoubleClick, nullptr); break;
      }
      return 0;
    case WM_COMMAND:
      OnMenu(LOWORD(w));
      return 0;
    case WM_ENDSESSION:
      if (w) SaveState();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      if (m == taskbarCreated_ && taskbarCreated_) {
        trayAdded_ = false;
        AddTray();
        return 0;
      }
  }
  return DefWindowProcW(h, m, w, l);
}

// ---- command dispatch ------------------------------------------------------
std::string App::Exec(const std::wstring& line, HWND replyTo) {
  auto a = SplitArgs(line);
  std::string reply;
  if (a.empty()) return reply;
  std::wstring verb = Lower(a[0]);
  while (!verb.empty() && (verb[0] == L'-' || verb[0] == L'/')) verb.erase(0, 1);
  auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::wstring(); };
  bool isSwitch = verb == L"monitor" || verb == L"display" || verb == L"tv" || verb == L"profile" || verb == L"toggle" ||
                  verb == L"next" || verb == L"primary";
  Log(L"command: %ls", line.c_str());
  if (isSwitch && wake_.active && verb != L"monitor" && verb != L"display" && verb != L"tv") CancelWake(false);
  if (isSwitch && switching_) {
    reply = "busy";
  } else if (isSwitch && lastSwitchEnd_ && NowMs() - lastSwitchEnd_ < (ULONGLONG)cfg_.debounceMs) {
    reply = "ignored: pressed again too quickly";
    Log(L"ignored (debounce)");
  } else if (verb == L"monitor" || verb == L"display") {
    reply = DoMonitor(arg(1), Lower(arg(2)));
  } else if (verb == L"tv") {
    reply = DoMonitor(L"", Lower(arg(1)));
  } else if (verb == L"profile") {
    reply = DoProfile(arg(1));
  } else if (verb == L"toggle") {
    reply = DoToggle(arg(1), arg(2));
  } else if (verb == L"next" || verb == L"cycle") {
    reply = DoNext();
  } else if (verb == L"primary") {
    reply = DoPrimary(arg(1));
  } else if (verb == L"audio" || verb == L"sound") {
    reply = DoAudio(arg(1), false);
  } else if (verb == L"mic") {
    reply = DoAudio(arg(1), true);
  } else if (verb == L"save") {
    reply = SaveProfile(arg(1), arg(1).empty());
  } else if (verb == L"delete") {
    reply = DeleteProfile(arg(1));
  } else if (verb == L"restore") {
    reply = RestoreNow();
  } else if (verb == L"status") {
    reply = StatusJson(false);
  } else if (verb == L"list") {
    reply = StatusJson(true);
  } else if (verb == L"diag") {
    reply = Diag();
    Log(L"diag:\r\n%ls", Widen(reply).c_str());
  } else if (verb == L"windows") {
    mons_ = QueryMonitors();
    MonMap mm = MonMap::Current(mons_);
    std::wstring s;
    for (auto& w : CaptureLayout(mm).wins) {
      const MonMap::M* m = mm.Get(w.monKey);
      const wchar_t* st = w.state == WinState::Maximized ? L"max" : w.state == WinState::Minimized ? L"min" : L"   ";
      s += Format(L"%ls %-14ls (%5ld,%5ld %4ldx%-4ld) %ls | %ls\r\n", st, m ? m->label.c_str() : L"?", w.normal.left,
                  w.normal.top, w.normal.right - w.normal.left, w.normal.bottom - w.normal.top, w.exe.c_str(),
                  w.title.substr(0, 60).c_str());
    }
    reply = Narrow(s);
  } else if (verb == L"reload") {
    ReloadConfig(false);
    reply = "ok";
  } else if (verb == L"hook") {
    // run a display's onTurnOn/onTurnOff commands without switching (to test a CEC/network setup)
    const MonitorCfg* mc = cfg_.MonitorByName(arg(1));
    bool on = IEquals(arg(2), L"on");
    if (!mc || (on ? mc->onTurnOn : mc->onTurnOff).empty()) {
      reply = "error: no onTurnOn/onTurnOff commands for that display in settings";
    } else {
      RunHooks(on ? mc->onTurnOn : mc->onTurnOff, mc->match, on ? L"on" : L"off");
      reply = "ok: running, output goes to " + Narrow(DataDir()) + "\\hooks.log";
    }
  } else if (verb == L"menu") {
    PostMessageW(wnd_, WM_APP_TRAY, 0, WM_CONTEXTMENU);  // posted, so the caller isn't held while it is open
    reply = "ok";
  } else if (verb == L"settings" || verb == L"config") {
    ShellOpen(ConfigPath());
    reply = "ok";
  } else if (verb == L"log") {
    ShellOpen(LogPath());
    reply = "ok";
  } else if (verb == L"folder") {
    ShellOpen(DataDir());
    reply = "ok";
  } else if (verb == L"osd") {
    osd_.Show(arg(1).empty() ? L"DisplayFlip" : arg(1), arg(2), OsdKind::Info, cfg_.osdMs, cfg_.osdPosition);
    reply = "ok";
  } else if (verb == L"subscribe") {
    if (replyTo && std::find(subs_.begin(), subs_.end(), replyTo) == subs_.end()) subs_.push_back(replyTo);
    reply = StatusJson(false);
  } else if (verb == L"streamdeck") {
    std::wstring msg;
    bool ok = InstallStreamDeckPlugin(&msg);
    osd_.Show(ok ? L"Stream Deck plugin installed" : L"Stream Deck plugin", msg, ok ? OsdKind::Ok : OsdKind::Error, 3500,
              cfg_.osdPosition);
    reply = Narrow(msg);
  } else if (verb == L"autostart") {
    SetAutostart(!IEquals(arg(1), L"off"));
    reply = AutostartEnabled() ? "autostart on" : "autostart off";
  } else if (verb == L"quit" || verb == L"exit") {
    reply = "bye";
    if (replyTo) {
      std::string r = reply;
      COPYDATASTRUCT cds{kCdReply, (DWORD)r.size(), (void*)r.data()};
      DWORD_PTR res;
      SendMessageTimeoutW(replyTo, WM_COPYDATA, (WPARAM)wnd_, (LPARAM)&cds, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &res);
      replyTo = nullptr;
    }
    DestroyWindow(wnd_);
  } else {
    reply = "unknown command: " + Narrow(verb);
  }
  if (replyTo) {
    COPYDATASTRUCT cds{kCdReply, (DWORD)reply.size(), (void*)reply.data()};
    DWORD_PTR res;
    SendMessageTimeoutW(replyTo, WM_COPYDATA, (WPARAM)wnd_, (LPARAM)&cds, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &res);
  }
  return reply;
}

std::wstring App::DefaultMonitorSpec() {
  for (auto& c : cfg_.monitors)
    if (!c.match.empty()) return c.match;
  for (auto& m : mons_)
    if (!m.primary) return m.label;
  return L"";
}

bool App::AudioIsOn(const MonitorCfg& mc, const LiveMonitor& m) {
  AudioDevice def = audio_.Default(false);
  if (def.id.empty()) return false;
  if (IEquals(mc.audio, L"auto")) return IEquals(def.desc, m.name);
  if (IsNone(mc.audio)) return false;
  AudioDevice d;
  return audio_.Find(mc.audio, false, d, true) && d.id == def.id;
}

std::string App::DoMonitor(const std::wstring& specIn, const std::wstring& actionIn) {
  mons_ = QueryMonitors();
  std::wstring spec = specIn.empty() ? DefaultMonitorSpec() : specIn;
  std::wstring action = actionIn.empty() ? L"toggle" : actionIn;
  if (wake_.active) {
    // pressed again while this display is still waking up: never mind, send it back to sleep
    const MonitorCfg* wmc = cfg_.MonitorByName(spec);
    bool same = false;
    for (auto& n : wake_.names) same = same || IEquals(n, spec) || (wmc && IEquals(n, wmc->match));
    if (same && action != L"on") {
      CancelWake(true);
      return "ok: cancelled";
    }
    CancelWake(false);
  }
  const LiveMonitor* found = FindMonitor(mons_, spec);
  if (!found) {
    // so deeply asleep that Windows can't see it: wake it with its onTurnOn hook, then turn it on
    const MonitorCfg* mc = cfg_.MonitorByName(spec);
    if (mc && !mc->onTurnOn.empty() && action != L"off") {
      StartWake({mc->match}, L"monitor " + QuoteArg(mc->match) + L" on", false);
      return "ok: waking";
    }
    if (mc && action == L"off") {
      RunHooks(mc->onTurnOff, mc->match, L"off");
      return "ok: already off";
    }
    if (cfg_.osd)
      osd_.Show(L"Can't find display \"" + spec + L"\"", L"Is it plugged in and powered on?", OsdKind::Error, 3000,
                cfg_.osdPosition);
    Log(L"monitor '%ls' not found", spec.c_str());
    return "error: display not found";
  }
  LiveMonitor mon = *found;
  const MonitorCfg* mc = cfg_.Monitor(mon);
  bool on = action == L"on" ? true : action == L"off" ? false : !mon.active;
  if (on == mon.active) {
    // Windows already has it that way, but the TV itself may not (its own remote, "no signal" screen...)
    if (mc) RunHooks(on ? mc->onTurnOn : mc->onTurnOff, mon.name, on ? L"on" : L"off");
    if (on && mc && !IsNone(mc->audio)) RequestAudio(IEquals(mc->audio, L"auto") ? L"monitor:" + mon.name : mc->audio, false);
    if (cfg_.osd) osd_.Show(mon.label + (on ? L" is on" : L" is off"), L"", OsdKind::Info, 1000, cfg_.osdPosition);
    return "ok: unchanged";
  }
  SwitchSpec s;
  s.title = mon.label + (on ? L" on" : L" off");
  s.expectTopologyChange = true;
  std::wstring key = mon.key;
  if (on) {
    bool hasMem = monMem_.count(key) > 0;
    PathSnap mem = hasMem ? monMem_[key] : PathSnap();
    s.apply = [key, hasMem, mem](std::wstring* err) { return EnableMonitor(key, hasMem ? &mem : nullptr, err); };
    if (mc) {
      if (IEquals(mc->audio, L"auto")) s.audioAfter = L"monitor:" + mon.name;
      else if (!IsNone(mc->audio)) s.audioAfter = mc->audio;
      s.micAfter = mc->mic;
      if (!RecentlyWoken(mon.name)) s.hooksBefore.push_back({mc->onTurnOn, mon.name, L"on"});
      if (!s.hooksBefore.empty() && !s.hooksBefore.back().cmds.empty())
        s.replay = L"monitor " + QuoteArg(mon.label) + L" on";
    }
    s.expectOn.push_back(key);
  } else {
    std::wstring pp;
    if (const LiveMonitor* p = FindMonitor(mons_, cfg_.preferredPrimary)) pp = p->key;
    s.apply = [key, pp](std::wstring* err) { return DisableMonitor(key, pp, err); };
    if (mc && AudioIsOn(*mc, mon)) s.audioBefore = L"previous";
    if (mc && !mc->mic.empty()) s.micAfter = L"previous";
    if (mc) s.hooksAfter.push_back({mc->onTurnOff, mon.name, L"off"});
  }
  return DoSwitch(s) ? "ok" : "error";
}

bool App::RecentlyWoken(const std::wstring& name) {
  auto it = recentWake_.find(Lower(name));
  return it != recentWake_.end() && NowMs() - it->second < 60000;
}

std::vector<std::wstring> App::AsleepDisplays() {
  std::vector<std::wstring> out;
  for (auto& c : cfg_.monitors)
    if (!c.onTurnOn.empty() && !FindMonitor(mons_, c.match)) out.push_back(c.match);
  return out;
}

void App::StartWake(const std::vector<std::wstring>& names, const std::wstring& command, bool applyAnyway) {
  int wait = 0;
  std::wstring who;
  for (auto& n : names) {
    if (const MonitorCfg* mc = cfg_.MonitorByName(n)) {
      RunHooks(mc->onTurnOn, n, L"on");
      wait = std::max(wait, mc->waitMs);
    }
    recentWake_[Lower(n)] = NowMs();
    who += (who.empty() ? L"" : L", ") + n;
  }
  wake_.active = true;
  wake_.names = names;
  wake_.command = command;
  wake_.applyAnyway = applyAnyway;
  wake_.since = NowMs();
  wake_.notBefore = 0;
  wake_.deadline = NowMs() + (ULONGLONG)std::max(wait, 1000);
  SetTimer(wnd_, T_WAKE, 250, nullptr);
  Log(L"waking %ls, then: %ls", who.c_str(), command.c_str());
  if (cfg_.osd) osd_.Show(L"Waking " + who + L"…", L"Press again to cancel", OsdKind::Info, wait + 1500, cfg_.osdPosition);
}

void App::StartRetry(const std::vector<std::wstring>& names, const std::wstring& command) {
  int wait = 0;
  std::wstring who;
  for (auto& n : names) {
    const MonitorCfg* mc = cfg_.MonitorByName(n);
    wait = std::max(wait, mc ? mc->waitMs : 15000);
    who += (who.empty() ? L"" : L", ") + n;
  }
  wake_.active = true;
  wake_.names = names;
  wake_.command = command;
  wake_.applyAnyway = false;
  wake_.since = NowMs();
  wake_.notBefore = NowMs() + (ULONGLONG)std::min(5000, std::max(wait / 2, 1000));
  wake_.deadline = NowMs() + (ULONGLONG)std::max(wait, 1000);
  SetTimer(wnd_, T_WAKE, 250, nullptr);
  Log(L"%ls not on yet, retrying in a moment: %ls", who.c_str(), command.c_str());
  if (cfg_.osd) osd_.Show(L"Waking " + who + L"…", L"Press again to cancel", OsdKind::Info, wait + 1500, cfg_.osdPosition);
}

void App::CancelWake(bool runOffHooks) {
  if (!wake_.active) return;
  KillTimer(wnd_, T_WAKE);
  wake_.active = false;
  Log(L"wake cancelled");
  if (!runOffHooks) return;
  for (auto& n : wake_.names) {
    if (const MonitorCfg* mc = cfg_.MonitorByName(n)) RunHooks(mc->onTurnOff, n, L"off");
    recentWake_.erase(Lower(n));
  }
  if (cfg_.osd) osd_.Show(L"Cancelled", L"", OsdKind::Info, 1000, cfg_.osdPosition);
}

void App::OnWakeTimer() {
  if (!wake_.active) { KillTimer(wnd_, T_WAKE); return; }
  if (NowMs() < wake_.notBefore) return;
  mons_ = QueryMonitors();
  bool all = true;
  for (auto& n : wake_.names) all = all && FindMonitor(mons_, n) != nullptr;
  if (!all && NowMs() < wake_.deadline) return;
  KillTimer(wnd_, T_WAKE);
  PendingWake w = wake_;
  wake_.active = false;
  if (all) {
    Log(w.notBefore ? L"retrying after %llu ms" : L"woken display(s) visible after %llu ms", NowMs() - w.since);
    Exec(w.command, nullptr);
  } else if (w.applyAnyway) {
    Log(L"display(s) did not wake in time, applying without them");
    Exec(w.command, nullptr);  // RecentlyWoken() keeps it from waiting again
  } else {
    std::wstring who;
    for (auto& n : w.names) who += (who.empty() ? L"" : L", ") + n;
    Log(L"%ls did not show up after waking", who.c_str());
    if (cfg_.osd)
      osd_.Show(who + L" didn't come on", L"Check the TV, and hooks.log in the data folder", OsdKind::Error, 4000,
                cfg_.osdPosition);
  }
}

std::wstring App::ResolveProfile(const std::wstring& name) {
  if (name.empty()) return L"";
  auto it = profiles_.find(Lower(name));
  if (it != profiles_.end()) return it->second.name;
  for (auto& n : ProfileNames())
    if (IStartsWith(n, name)) return n;
  return L"";
}

std::string App::DoProfile(const std::wstring& nameIn) {
  std::wstring name = ResolveProfile(nameIn);
  if (name.empty()) {
    if (cfg_.osd)
      osd_.Show(L"No profile named \"" + nameIn + L"\"", L"Tray menu > Save current layout as profile", OsdKind::Error, 3000,
                cfg_.osdPosition);
    return "error: no such profile";
  }
  const ProfileData& pd = profiles_[Lower(name)];
  const ProfileCfg* pc = cfg_.Profile(name);
  mons_ = QueryMonitors();
  SwitchSpec s;
  s.title = name;
  s.profile = name;
  s.leavingProfile = activeProfile_.empty() ? lastProfile_ : activeProfile_;
  std::set<std::wstring> willBeOn, nowOn;
  for (auto& m : mons_)
    if (m.active) nowOn.insert(m.key);
  if (pd.hasSnap) {
    DisplaySnap cur;
    CaptureSnap(cur);
    std::set<std::wstring> connected;
    for (auto& m : mons_) connected.insert(m.key);
    // displays this profile needs that are asleep beyond Windows' sight: wake them first
    std::vector<std::wstring> wake;
    for (auto& p : pd.snap.paths) {
      if (connected.count(p.key) || p.name.empty()) continue;
      const MonitorCfg* mc = cfg_.MonitorByName(p.name);
      if (mc && !mc->onTurnOn.empty() && !RecentlyWoken(mc->match)) wake.push_back(mc->match);
    }
    if (!wake.empty()) {
      StartWake(wake, L"profile " + QuoteArg(name), true);
      return "ok: waking";
    }
    for (auto& p : pd.snap.paths)
      if (connected.count(p.key)) willBeOn.insert(p.key);
    if (!SnapMatchesCurrent(pd.snap, cur, mons_)) {
      DisplaySnap snap = pd.snap;
      s.apply = [snap](std::wstring* err) { return ApplySnap(snap, err); };
      s.expectTopologyChange = willBeOn != nowOn;
    }
  } else {
    willBeOn = nowOn;
  }
  for (auto& m : mons_) {
    const MonitorCfg* mc = cfg_.Monitor(m);
    if (!mc || !mc->HasHooks()) continue;
    bool willOn = willBeOn.count(m.key) > 0;
    if (!m.active && willOn && !RecentlyWoken(m.name)) s.hooksBefore.push_back({mc->onTurnOn, m.name, L"on"});
    if (m.active && !willOn) s.hooksAfter.push_back({mc->onTurnOff, m.name, L"off"});
  }
  for (auto& m : mons_)
    if (!m.active && willBeOn.count(m.key)) s.expectOn.push_back(m.key);
  for (auto& h : s.hooksBefore)
    if (!h.cmds.empty()) s.replay = L"profile " + QuoteArg(name);
  AudioDevice def = audio_.Default(false);
  if (pc && !IsNone(pc->audio)) {
    AudioDevice d;
    if (IEquals(pc->audio, L"previous") || audio_.Find(pc->audio, false, d, true)) s.audioBefore = pc->audio;
    else s.audioAfter = pc->audio;  // appears once the displays are on (HDMI audio)
  } else {
    for (auto& m : mons_) {
      const MonitorCfg* mc = cfg_.Monitor(m);
      if (!mc || IsNone(mc->audio)) continue;
      bool willOn = willBeOn.count(m.key) > 0;
      if (m.active && !willOn && AudioIsOn(*mc, m)) s.audioBefore = L"previous";
      if (!m.active && willOn && s.audioAfter.empty())
        s.audioAfter = IEquals(mc->audio, L"auto") ? L"monitor:" + m.name : mc->audio;
    }
  }
  if (pc && !pc->mic.empty()) s.micAfter = pc->mic;
  lastProfile_ = name;
  return DoSwitch(s) ? "ok" : "error";
}

std::string App::DoToggle(const std::wstring& aIn, const std::wstring& bIn) {
  std::wstring a = ResolveProfile(aIn), b = ResolveProfile(bIn);
  if (a.empty() || b.empty()) return DoProfile(a.empty() ? aIn : bIn);  // reports the missing one
  std::wstring cur = activeProfile_;
  std::wstring target = IEquals(cur, a) ? b : IEquals(cur, b) ? a : (IEquals(lastProfile_, a) ? b : a);
  return DoProfile(target);
}

std::string App::DoNext() {
  auto names = ProfileNames();
  if (names.empty()) return DoProfile(L"");
  std::wstring cur = activeProfile_.empty() ? lastProfile_ : activeProfile_;
  size_t idx = 0;
  for (size_t i = 0; i < names.size(); i++)
    if (IEquals(names[i], cur)) idx = (i + 1) % names.size();
  return DoProfile(names[idx]);
}

std::string App::DoPrimary(const std::wstring& spec) {
  mons_ = QueryMonitors();
  const LiveMonitor* m = FindMonitor(mons_, spec);
  if (!m || !m->active) {
    if (cfg_.osd) osd_.Show(L"Can't make \"" + spec + L"\" the main display", L"It is not on", OsdKind::Error, 2500, cfg_.osdPosition);
    return "error: display not on";
  }
  SwitchSpec s;
  s.title = m->label + L" is now the main display";
  std::wstring key = m->key;
  s.apply = [key](std::wstring* err) { return SetPrimaryMonitor(key, err); };
  return DoSwitch(s) ? "ok" : "error";
}

std::string App::DoAudio(const std::wstring& spec, bool capture) {
  std::wstring msg;
  bool ok = ApplyAudio(spec, capture, &msg);
  if (cfg_.osd) {
    if (ok) osd_.Show(capture ? L"Microphone" : L"Sound", msg, OsdKind::Ok, 1200, cfg_.osdPosition);
    else osd_.Show(L"Sound device not found", spec, OsdKind::Error, 2500, cfg_.osdPosition);
  }
  Broadcast();
  return ok ? "ok" : "error: device not found";
}

// ---- the switch itself -----------------------------------------------------
bool App::DoSwitch(const SwitchSpec& s) {
  if (switching_) return false;
  switching_ = true;
  switchStart32_ = GetTickCount();
  ULONGLONG t0 = NowMs();
  KillTimer(wnd_, T_PASS);
  plan_.clear();

  // 1) remember the "before" state
  mons_ = QueryMonitors();
  std::wstring oldTopo = TopologyKey(mons_);
  std::set<std::wstring> oldMons;
  for (auto& m : mons_)
    if (m.active) oldMons.insert(m.key);
  Layout before;
  if (cfg_.remember) {
    before = CaptureLayout(MonMap::Current(mons_));
    memory_[oldTopo] = before;
  }
  RefreshMonitorMemory();
  ULONGLONG t1 = NowMs();

  // 2) sound that is already available switches first (instant, before any screen blanks)
  std::wstring audioMsg;
  if (!s.audioBefore.empty() && ApplyAudio(s.audioBefore, false, &audioMsg) && s.audioBefore == L"previous")
    audioOnMonitor_.clear();
  if (!s.leavingProfile.empty() && !IEquals(s.leavingProfile, s.profile))
    if (const ProfileCfg* lp = cfg_.Profile(s.leavingProfile)) CloseApps(lp->closeOnLeave);
  for (auto& h : s.hooksBefore) {  // wake TVs (CEC/network) while Windows brings the output up
    RunHooks(h.cmds, h.display, h.event.c_str());
    if (!h.cmds.empty()) recentWake_[Lower(h.display)] = NowMs();
  }

  // 3) displays
  bool ok = true;
  std::wstring err;
  if (s.apply) {
    if (cfg_.osd) osd_.Show(s.title, L"Switching\u2026", OsdKind::Info, 8000, cfg_.osdPosition);
    suppressUntil_ = NowMs() + 5000;
    ok = s.apply(&err);
  }
  ULONGLONG t2 = NowMs();
  if (ok)  // e.g. put the TV into standby now that nothing is shown on it
    for (auto& h : s.hooksAfter) RunHooks(h.cmds, h.display, h.event.c_str());

  // 4) wait (briefly) until the desktop reflects the new topology
  mons_ = QueryMonitors();
  std::wstring newTopo = TopologyKey(mons_);
  auto notOn = [&]() {
    std::vector<std::wstring> r;
    for (auto& k : s.expectOn) {
      bool on = false;
      for (auto& m : mons_) on = on || (m.key == k && m.active);
      if (!on) r.push_back(k);
    }
    return r;
  };
  for (int i = 0; ok && i < 30 && ((s.expectTopologyChange && newTopo == oldTopo) || !notOn().empty()); i++) {
    Sleep(20);
    mons_ = QueryMonitors();
    newTopo = TopologyKey(mons_);
  }
  // Windows accepted the layout but a display never lit up: almost always a TV in standby,
  // which ignores the picture (a PC can't power a TV on over plain HDMI)
  std::vector<std::wstring> missingLabels, missingNames;
  if (ok)
    for (auto& k : notOn())
      for (auto& m : mons_)
        if (m.key == k) { missingLabels.push_back(m.label); missingNames.push_back(m.name); }
  bool retrying = !missingLabels.empty() && !s.replay.empty();
  if (!missingLabels.empty() && !retrying && newTopo == oldTopo) ok = false;  // nothing changed at all
  topo_ = newTopo;
  fingerprint_ = Fingerprint();
  RefreshMonitorMemory();

  // 5) windows: bring back what lived on returning displays, keep everything else put
  int back = 0;
  if (ok && cfg_.remember && newTopo != oldTopo) {
    MonMap mm = MonMap::Current(mons_);
    auto it = memory_.find(newTopo);
    plan_ = PlanRestore(before, it == memory_.end() ? nullptr : &it->second, oldMons, mm, cfg_.orphans, true);
    for (auto& t : plan_)
      if (!t.protect && mm.Has(t.want.monKey) && !oldMons.count(t.want.monKey)) back++;
    restoreFg_ = before.foreground;
  }
  ULONGLONG t3 = NowMs();
  if (ok) {
    std::set<HWND> ruled = ApplyRules(nullptr, false);
    for (auto& t : plan_)
      if (ruled.count(t.hwnd)) t.hwnd = nullptr;  // explicit rules beat memory
  }
  if (!plan_.empty()) {
    ApplyTargets(plan_, MonMap::Current(mons_));
    passIdx_ = 0;
    SetTimer(wnd_, T_PASS, kPassDelays[0], nullptr);
    lastPlan_ = plan_;
  }

  // 6) sound/mic that only exists after the switch (a TV's HDMI audio shows up a moment later)
  bool audioOnMissing = false;
  for (auto& n : missingNames) audioOnMissing = audioOnMissing || IEquals(s.audioAfter, L"monitor:" + n);
  if (ok) {
    if (!s.audioAfter.empty() && !audioOnMissing) {
      RequestAudio(s.audioAfter, false);
      if (IStartsWith(s.audioAfter, L"monitor:")) audioOnMonitor_ = s.audioAfter.substr(8);
    }
    if (!s.micAfter.empty()) RequestAudio(s.micAfter, true);
    if (!s.profile.empty()) ProfileExtras(s.profile);
  }
  activeProfile_ = DetectProfile();
  switching_ = false;
  lastSwitchEnd_ = NowMs();
  switchEnd32_ = GetTickCount();
  suppressUntil_ = NowMs() + 2500;
  freezeSnapshotUntil_ = NowMs() + 3000;

  // 7) feedback
  std::wstring detail;
  if (!ok) {
    detail = err;
  } else {
    std::vector<std::wstring> parts;
    if (pending_[0].active) parts.push_back(L"Sound: connecting\u2026");
    else if (!audioMsg.empty()) parts.push_back(L"Sound: " + audioMsg);
    else if (!s.audioAfter.empty()) parts.push_back(L"Sound: " + audio_.Default(false).name);
    if (back) parts.push_back(Format(L"%d window%ls back", back, back == 1 ? L"" : L"s"));
    if (s.apply) parts.push_back(Format(L"%.1f s", (NowMs() - t0) / 1000.0));
    for (size_t i = 0; i < parts.size(); i++) detail += (i ? L"  \u00b7  " : L"") + parts[i];
  }
  std::wstring who;
  for (auto& l : missingLabels) who += (who.empty() ? L"" : L", ") + l;
  if (retrying) {
    StartRetry(missingLabels, s.replay);
  } else if (!missingLabels.empty()) {
    Log(L"%ls did not turn on (TV in standby?)", who.c_str());
    if (cfg_.osd)
      osd_.Show(who + L" didn't turn on", L"Is the TV in standby? Turn it on with its remote, then press again",
                ok ? OsdKind::Warn : OsdKind::Error, 5000, cfg_.osdPosition);
  } else if (cfg_.osd) {
    osd_.Show(s.title, detail, ok ? OsdKind::Ok : OsdKind::Error, ok ? cfg_.osdMs : 4500, cfg_.osdPosition);
  }
  Log(L"switch '%ls' %ls: capture %llu ms, displays %llu ms, windows %llu ms, total %llu ms, %d windows back%ls%ls",
      s.title.c_str(), ok ? L"ok" : L"FAILED", t1 - t0, t2 - t1, t3 - t2, NowMs() - t0, back, ok ? L"" : L": ",
      err.c_str());
  UpdateTray();
  Broadcast();
  MarkDirty();
  return ok;
}

void App::RunPass() {
  KillTimer(wnd_, T_PASS);
  if (plan_.empty()) return;
  int moved = ApplyTargets(plan_, MonMap::Current(mons_));
  if (moved) Log(L"restore pass %d moved %d window(s)", passIdx_ + 1, moved);
  if (restoreFg_ && IsWindow(restoreFg_) && GetForegroundWindow() != restoreFg_) SetForegroundWindow(restoreFg_);
  passIdx_++;
  if (passIdx_ < (int)(sizeof(kPassDelays) / sizeof(kPassDelays[0]))) {
    SetTimer(wnd_, T_PASS, kPassDelays[passIdx_], nullptr);
  } else {
    plan_.clear();
    restoreFg_ = nullptr;
  }
}

std::string App::RestoreNow() {
  if (lastPlan_.empty()) {
    if (cfg_.osd) osd_.Show(L"Nothing to put back", L"", OsdKind::Info, 1200, cfg_.osdPosition);
    return "ok: nothing to do";
  }
  plan_ = lastPlan_;
  int moved = ApplyTargets(plan_, MonMap::Current(mons_));
  passIdx_ = 0;
  SetTimer(wnd_, T_PASS, kPassDelays[0], nullptr);
  if (cfg_.osd) osd_.Show(L"Windows put back", Format(L"%d moved", moved), OsdKind::Ok, 1200, cfg_.osdPosition);
  return "ok";
}

// ---- audio -----------------------------------------------------------------
bool App::ResolveAudio(const std::wstring& spec, bool capture, AudioDevice& out) {
  if (IEquals(spec, L"previous")) {
    const std::wstring& prev = capture ? micPrevId_ : audioPrevId_;
    if (!prev.empty() && audio_.Find(prev, capture, out, true)) return true;
    if (!capture)
      for (auto& f : cfg_.audioFallback)
        if (audio_.Find(f, false, out, true)) return true;
    return false;
  }
  if (IStartsWith(spec, L"monitor:")) return !capture && audio_.FindForMonitor(spec.substr(8), out);
  return audio_.Find(spec, capture, out, true);
}

bool App::ApplyAudio(const std::wstring& spec, bool capture, std::wstring* msg) {
  AudioDevice d;
  if (!ResolveAudio(spec, capture, d)) return false;
  if (msg) *msg = d.name;
  AudioDevice cur = audio_.Default(capture);
  if (cur.id == d.id) return true;
  if (!IEquals(spec, L"previous") && !cur.id.empty()) {
    (capture ? micPrevId_ : audioPrevId_) = cur.id;  // where "previous" goes back to
    MarkDirty();
  }
  bool ok = audio_.SetDefault(d.id, capture, cfg_.commsToo);
  Log(L"%ls: '%ls' -> '%ls' %ls", capture ? L"mic" : L"sound", cur.name.c_str(), d.name.c_str(), ok ? L"ok" : L"FAILED");
  return ok;
}

void App::RequestAudio(const std::wstring& spec, bool capture) {
  std::wstring msg;
  if (ApplyAudio(spec, capture, &msg)) return;
  PendingAudio& p = pending_[capture ? 1 : 0];
  p.active = true;
  p.spec = spec;
  p.since = NowMs();
  p.deadline = NowMs() + (ULONGLONG)cfg_.audioWaitMs;
  SetTimer(wnd_, T_AUDIO, 150, nullptr);  // device notifications usually beat this
}

void App::TryPendingAudio() {
  bool waiting = false;
  for (int c = 0; c < 2; c++) {
    PendingAudio& p = pending_[c];
    if (!p.active) continue;
    std::wstring msg;
    if (ApplyAudio(p.spec, c == 1, &msg)) {
      p.active = false;
      Log(L"%ls device ready after %llu ms", c ? L"mic" : L"sound", NowMs() - p.since);
      if (cfg_.osd && c == 0) {
        if (osd_.Visible()) osd_.SetDetail(L"Sound: " + msg);
        else osd_.Show(L"Sound", msg, OsdKind::Ok, 1200, cfg_.osdPosition);
      }
      Broadcast();
    } else if (NowMs() > p.deadline) {
      p.active = false;
      std::wstring what = IStartsWith(p.spec, L"monitor:") ? p.spec.substr(8) + L" audio" : p.spec;
      Log(L"gave up waiting for %ls", what.c_str());
      if (cfg_.osd) osd_.Show(L"Sound device didn't show up", what, OsdKind::Warn, 3000, cfg_.osdPosition);
    } else {
      waiting = true;
    }
  }
  if (!waiting) KillTimer(wnd_, T_AUDIO);
}

// ---- state -----------------------------------------------------------------
void App::RefreshMonitorMemory() {
  DisplaySnap cur;
  if (!CaptureSnap(cur)) return;
  for (auto& p : cur.paths) monMem_[p.key] = p;
}

void App::LoadProfiles() {
  profiles_.clear();
  WIN32_FIND_DATAW fd;
  std::wstring dir = DataDir() + L"\\profiles\\";
  HANDLE f = FindFirstFileW((dir + L"*.json").c_str(), &fd);
  if (f != INVALID_HANDLE_VALUE) {
    do {
      std::string text;
      if (!ReadFileUtf8(dir + fd.cFileName, text)) continue;
      Json j = Json::Parse(text);
      std::wstring name = j.str("name");
      if (name.empty()) {
        name = fd.cFileName;
        name = name.substr(0, name.size() - 5);
      }
      ProfileData pd;
      pd.name = name;
      pd.snap = DisplaySnap::FromJson(j);
      pd.hasSnap = !pd.snap.paths.empty();
      profiles_[Lower(name)] = pd;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
  }
  for (auto& pc : cfg_.profiles)  // settings-only profiles (sound/apps, no display change)
    if (!profiles_.count(Lower(pc.name))) profiles_[Lower(pc.name)] = ProfileData{pc.name, {}, false};
}

std::vector<std::wstring> App::ProfileNames() {
  std::vector<std::wstring> out;
  for (auto& pc : cfg_.profiles)
    if (profiles_.count(Lower(pc.name))) out.push_back(profiles_[Lower(pc.name)].name);
  for (auto& kv : profiles_) {
    bool listed = false;
    for (auto& n : out)
      if (IEquals(n, kv.second.name)) listed = true;
    if (!listed) out.push_back(kv.second.name);
  }
  return out;
}

std::wstring App::DetectProfile() {
  DisplaySnap cur;
  if (!CaptureSnap(cur)) return L"";
  auto matches = [&](const std::wstring& n) {
    auto it = profiles_.find(Lower(n));
    if (it == profiles_.end()) return false;
    return it->second.hasSnap ? SnapMatchesCurrent(it->second.snap, cur, mons_) : false;
  };
  if (!lastProfile_.empty()) {
    auto it = profiles_.find(Lower(lastProfile_));
    if (it != profiles_.end() && (!it->second.hasSnap || matches(lastProfile_))) return it->second.name;
  }
  for (auto& n : ProfileNames())
    if (matches(n)) return n;
  return L"";
}

void App::ReloadConfig(bool initial) {
  Config c;
  std::wstring err;
  cfgTime_ = FileWriteTime(ConfigPath());
  if (LoadConfig(ConfigPath(), c, &err)) {
    cfg_ = c;
    if (!initial) Log(L"settings reloaded");
  } else {
    Log(L"settings error: %ls", err.c_str());
    osd_.Show(L"Settings file has an error", err, OsdKind::Error, 5000, cfg_.osdPosition);
    if (!initial) return;  // keep running with the last good settings
  }
  LoadProfiles();
  RegisterHotkeys();
  UpdateRuleHook();
  if (!initial && !hotkeyErrors_.empty() && cfg_.osd)
    osd_.Show(L"Hotkey problem", hotkeyErrors_[0], OsdKind::Warn, 4000, cfg_.osdPosition);
  UpdateTray();
  Broadcast();
}

void App::RegisterHotkeys() {
  for (size_t i = 0; i < hotkeyCmds_.size(); i++) UnregisterHotKey(wnd_, kHotkeyBase + (int)i);
  hotkeyCmds_.clear();
  hotkeyErrors_.clear();
  auto add = [&](const std::wstring& combo, const std::wstring& cmd) {
    UINT mods, vk;
    int id = kHotkeyBase + (int)hotkeyCmds_.size();
    hotkeyCmds_.push_back(cmd);
    if (!ParseHotkey(combo, mods, vk)) {
      hotkeyErrors_.push_back(L"Can't read hotkey \"" + combo + L"\"");
    } else if (!RegisterHotKey(wnd_, id, mods | MOD_NOREPEAT, vk)) {
      hotkeyErrors_.push_back(combo + L" is already taken by another app");
    } else {
      Log(L"hotkey %ls -> %ls", combo.c_str(), cmd.c_str());
      return;
    }
    Log(L"hotkey problem: %ls", hotkeyErrors_.back().c_str());
  };
  for (auto& hk : cfg_.hotkeys) add(hk.first, hk.second);
  for (auto& p : cfg_.profiles)
    if (!p.hotkey.empty()) add(p.hotkey, L"profile " + QuoteArg(p.name));
}

void App::LoadState() {
  std::string text;
  if (!ReadFileUtf8(DataDir() + L"\\state.json", text)) return;
  Json j = Json::Parse(text);
  lastProfile_ = j.str("lastProfile");
  const Json& a = j["audio"];
  audioPrevId_ = a.str("previous");
  micPrevId_ = a.str("previousMic");
  audioOnMonitor_ = a.str("onMonitor");
  for (auto& kv : j["monitors"].items()) monMem_[Widen(kv.first)] = PathSnap::FromJson(kv.second);
  // window handles are only meaningful within the same Windows session
  long long boot = (long long)j.num("bootId");
  if (std::llabs(boot - BootId()) <= 2)
    for (auto& kv : j["layouts"].items()) memory_[Widen(kv.first)] = Layout::FromJson(kv.second);
}

void App::SaveState() {
  Json j = Json::Obj();
  j.set("version", Json(1));
  j.set("bootId", Json((long long)BootId()));
  j.set("lastProfile", Json(lastProfile_));
  Json a = Json::Obj();
  a.set("previous", Json(audioPrevId_));
  a.set("previousMic", Json(micPrevId_));
  a.set("onMonitor", Json(audioOnMonitor_));
  j.set("audio", a);
  Json mm = Json::Obj();
  for (auto& kv : monMem_) mm.set(Narrow(kv.first), kv.second.ToJson());
  j.set("monitors", mm);
  Json lay = Json::Obj();
  for (auto& kv : memory_) lay.set(Narrow(kv.first), kv.second.ToJson());
  j.set("layouts", lay);
  WriteFileUtf8(DataDir() + L"\\state.json", j.Dump(false));
}

// ---- unplanned display changes (sleep/wake, cables, Win+P, other tools) ----
void App::OnDisplayChange() {
  if (switching_) return;
  freezeSnapshotUntil_ = std::max(freezeSnapshotUntil_, NowMs() + 3000);
  // debounce (displays often flap a few times); right after our own switch, check once the dust settles
  SetTimer(wnd_, T_DISPCHANGE, NowMs() < suppressUntil_ ? 3500 : 700, nullptr);
}

void App::HandleUnplanned() {
  if (switching_) return;
  // nothing structural changed (count/size of the desktop): skip the display queries,
  // which can block for a second or two while the GPU driver is still busy
  if (Fingerprint() == fingerprint_) return;
  std::set<std::wstring> oldMons;
  std::vector<LiveMonitor> old = mons_;
  for (auto& m : mons_)
    if (m.active) oldMons.insert(m.key);
  std::wstring oldTopo = topo_;
  mons_ = QueryMonitors();
  topo_ = TopologyKey(mons_);
  fingerprint_ = Fingerprint();
  RefreshMonitorMemory();
  if (topo_ != oldTopo && NowMs() >= suppressUntil_) {
    Log(L"displays changed outside DisplayFlip (%u -> %u on)", (unsigned)oldMons.size(),
        (unsigned)MonMap::Current(mons_).mons.size());
    if (cfg_.remember && cfg_.fixUnplanned) {
      Layout before = memory_.count(oldTopo) ? memory_[oldTopo] : Layout();
      auto it = memory_.find(topo_);
      MonMap mm = MonMap::Current(mons_);
      plan_ = PlanRestore(before, it == memory_.end() ? nullptr : &it->second, oldMons, mm, cfg_.orphans, false);
      if (!plan_.empty()) {
        Log(L"putting %u window(s) back", (unsigned)plan_.size());
        ApplyTargets(plan_, mm);
        passIdx_ = 0;
        restoreFg_ = GetForegroundWindow();
        SetTimer(wnd_, T_PASS, kPassDelays[0], nullptr);
      }
    }
    // the display carrying our sound vanished on its own: go back to the previous device
    if (!audioOnMonitor_.empty()) {
      bool still = false;
      for (auto& m : mons_)
        if (m.active && IEquals(m.name, audioOnMonitor_)) still = true;
      if (!still) {
        audioOnMonitor_.clear();
        ApplyAudio(L"previous", false, nullptr);
      }
    }
  }
  activeProfile_ = DetectProfile();
  UpdateTray();
  Broadcast();
  MarkDirty();
}

void App::Snapshot() {
  if (switching_ || !cfg_.remember || NowMs() < freezeSnapshotUntil_ || !plan_.empty()) return;
  if (Fingerprint() != fingerprint_) {  // a change is in flight; let the display handler settle it first
    OnDisplayChange();
    return;
  }
  memory_[topo_] = CaptureLayout(MonMap::Current(mons_));
  for (auto it = ruleSeen_.begin(); it != ruleSeen_.end();) it = IsWindow(*it) ? std::next(it) : ruleSeen_.erase(it);
}

// ---- window rules ----------------------------------------------------------
void App::UpdateRuleHook() {
  bool want = false;
  for (auto& r : cfg_.rules)
    if (r.onOpen) want = true;
  if (want && !hookShow_) {
    hookShow_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, WinEventThunk, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  } else if (!want && hookShow_) {
    UnhookWinEvent(hookShow_);
    hookShow_ = nullptr;
  }
}

void App::OnWinEvent(DWORD ev, HWND h, LONG idObj, LONG idChild) {
  if (!h) return;
  if (ev == EVENT_SYSTEM_MOVESIZESTART) {
    for (auto& t : plan_)  // the user grabbed it: stop putting it back
      if (t.hwnd == h) t.hwnd = nullptr;
    return;
  }
  if (ev == EVENT_OBJECT_SHOW && idObj == OBJID_WINDOW && idChild == CHILDID_SELF && GetAncestor(h, GA_ROOT) == h &&
      !ruleSeen_.count(h)) {
    rulePending_.push_back({h, NowMs() + 150});  // give the app a moment to place itself first
    SetTimer(wnd_, T_RULES, 60, nullptr);
  }
}

void App::ProcessPendingRules() {
  ULONGLONG now = NowMs();
  bool left = false;
  for (size_t i = 0; i < rulePending_.size();) {
    if (rulePending_[i].second > now) { left = true; i++; continue; }
    HWND h = rulePending_[i].first;
    rulePending_.erase(rulePending_.begin() + i);
    if (ruleSeen_.count(h) || !IsWindow(h) || !IsManagedWindow(h)) continue;
    ruleSeen_.insert(h);
    ApplyRules(h, true);
  }
  if (!left) KillTimer(wnd_, T_RULES);
}

static BOOL CALLBACK CollectManaged(HWND h, LPARAM lp) {
  if (IsManagedWindow(h)) ((std::vector<HWND>*)lp)->push_back(h);
  return TRUE;
}

std::set<HWND> App::ApplyRules(HWND only, bool onOpen) {
  std::set<HWND> matched;
  if (cfg_.rules.empty()) return matched;
  MonMap mm = MonMap::Current(mons_);
  std::vector<HWND> wins;
  if (only) wins.push_back(only);
  else EnumWindows(CollectManaged, (LPARAM)&wins);
  std::map<DWORD, std::wstring> exes;
  for (HWND h : wins) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    auto e = exes.find(pid);
    if (e == exes.end()) e = exes.emplace(pid, ProcessImageName(pid)).first;
    wchar_t title[256] = {}, cls[256] = {};
    GetWindowTextW(h, title, 256);
    GetClassNameW(h, cls, 256);
    for (auto& r : cfg_.rules) {
      if (onOpen ? !r.onOpen : !r.onSwitch) continue;
      if (!r.profiles.empty()) {
        bool in = false;
        for (auto& p : r.profiles) in = in || IEquals(p, activeProfile_);
        if (!in) continue;
      }
      if (!r.app.empty() && !WildMatch(r.app, e->second)) continue;
      if (!r.title.empty() && !WildMatch(r.title, title)) continue;
      if (!r.cls.empty() && !WildMatch(r.cls, cls)) continue;
      matched.insert(h);
      const LiveMonitor* m = FindMonitor(mons_, r.monitor);
      if (m && m->active && PlaceWindow(h, mm, m->key, r.state, r.hasRect ? &r.rect : nullptr))
        Log(L"rule: %ls '%ls' -> %ls", e->second.c_str(), title, m->label.c_str());
      break;  // first matching rule wins
    }
  }
  return matched;
}

// ---- profile extras --------------------------------------------------------
void App::ProfileExtras(const std::wstring& name) {
  const ProfileCfg* pc = cfg_.Profile(name);
  if (!pc) return;
  for (auto& l : pc->launch) {
    auto a = SplitArgs(l);
    if (a.empty()) continue;
    std::wstring exe = a[0];
    size_t k = exe.find_last_of(L"\\/");
    std::wstring exeName = k == std::wstring::npos ? exe : exe.substr(k + 1);
    if (IsProcessRunning(exeName)) continue;
    std::wstring args;
    for (size_t i = 1; i < a.size(); i++) args += (i > 1 ? L" " : L"") + QuoteArg(a[i]);
    std::wstring dir = k == std::wstring::npos ? L"" : exe.substr(0, k);
    ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), dir.empty() ? nullptr : dir.c_str(),
                  SW_SHOWNORMAL);
    Log(L"launched %ls", exe.c_str());
  }
  for (auto& r : pc->run) LaunchDetached(L"cmd.exe /c " + r, true);
  if (IEquals(pc->cursor, L"primary"))
    for (auto& m : mons_)
      if (m.primary) SetCursorPos((m.rect.left + m.rect.right) / 2, (m.rect.top + m.rect.bottom) / 2);
}

struct CloseCtx {
  const std::vector<std::wstring>* exes;
  int closed;
};

static BOOL CALLBACK CloseProc(HWND h, LPARAM lp) {
  CloseCtx* c = (CloseCtx*)lp;
  if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  std::wstring exe = ProcessImageName(pid);
  for (auto& e : *c->exes)
    if (IEquals(e, exe)) {
      PostMessageW(h, WM_CLOSE, 0, 0);
      c->closed++;
    }
  return TRUE;
}

void App::CloseApps(const std::vector<std::wstring>& exes) {
  if (exes.empty()) return;
  CloseCtx c{&exes, 0};
  EnumWindows(CloseProc, (LPARAM)&c);
  Log(L"asked %d window(s) to close", c.closed);
}

// ---- profiles: save / delete -----------------------------------------------
std::string App::SaveProfile(std::wstring name, bool prompt) {
  if (prompt) {
    auto names = ProfileNames();
    name = activeProfile_.empty() ? Format(L"Profile %u", (unsigned)names.size() + 1) : activeProfile_;
    if (!PromptText(inst_, L"DisplayFlip", L"Save the current display layout as:", name)) return "cancelled";
  }
  name = Trim(name);
  if (name.empty()) return "error: no name";
  DisplaySnap cur;
  if (!CaptureSnap(cur)) return "error: could not read displays";
  Json j = Json::Obj();
  j.set("name", Json(name));
  j.set("displays", cur.ToJson()["displays"]);
  std::wstring path = DataDir() + L"\\profiles\\" + SafeFileName(name) + L".json";
  if (!WriteFileUtf8(path, j.Dump(true))) return "error: could not write the profile";
  LoadProfiles();
  lastProfile_ = name;
  activeProfile_ = DetectProfile();
  Log(L"saved profile '%ls': %ls", name.c_str(), cur.Describe().c_str());
  if (cfg_.osd) osd_.Show(L"Saved profile \"" + name + L"\"", cur.Describe(), OsdKind::Ok, 2500, cfg_.osdPosition);
  UpdateTray();
  Broadcast();
  MarkDirty();
  return "ok";
}

std::string App::DeleteProfile(const std::wstring& nameIn) {
  std::wstring name = ResolveProfile(nameIn);
  if (name.empty()) return "error: no such profile";
  DeleteFileW((DataDir() + L"\\profiles\\" + SafeFileName(name) + L".json").c_str());
  LoadProfiles();
  if (IEquals(lastProfile_, name)) lastProfile_.clear();
  activeProfile_ = DetectProfile();
  if (cfg_.osd) osd_.Show(L"Deleted profile \"" + name + L"\"", L"", OsdKind::Info, 1500, cfg_.osdPosition);
  UpdateTray();
  Broadcast();
  return "ok";
}

// ---- status / diag ---------------------------------------------------------
std::string App::StatusJson(bool withAudio) {
  Json j = Json::Obj();
  j.set("version", Json(DF_VERSION_A));
  j.set("profile", Json(activeProfile_));
  j.set("lastProfile", Json(lastProfile_));
  j.set("busy", Json(switching_));
  Json ms = Json::Arr();
  for (auto& m : mons_) {
    Json o = Json::Obj();
    o.set("label", Json(m.label));
    o.set("name", Json(m.name));
    o.set("key", Json(m.key));
    o.set("active", Json(m.active));
    o.set("primary", Json(m.primary));
    ms.push(o);
  }
  for (auto& n : AsleepDisplays()) {
    Json o = Json::Obj();
    o.set("label", Json(n));
    o.set("name", Json(n));
    o.set("key", Json(""));
    o.set("active", Json(false));
    o.set("primary", Json(false));
    o.set("asleep", Json(true));
    ms.push(o);
  }
  j.set("monitors", ms);
  j.set("waking", Json(wake_.active));
  Json ps = Json::Arr();
  for (auto& n : ProfileNames()) ps.push(Json(n));
  j.set("profiles", ps);
  j.set("defaultMonitor", Json(DefaultMonitorSpec()));
  j.set("audio", Json(audio_.Default(false).name));
  if (withAudio) {
    Json out = Json::Arr(), in = Json::Arr();
    for (auto& d : audio_.List(false)) out.push(Json(d.name));
    for (auto& d : audio_.List(true)) in.push(Json(d.name));
    j.set("playback", out);
    j.set("recording", in);
  }
  return j.Dump(false);
}

std::string App::Diag() {
  mons_ = QueryMonitors();
  std::wstring s = L"DisplayFlip " DF_VERSION L"\r\n\r\nDisplays:\r\n";
  for (auto& m : mons_) {
    s += Format(L"  [%ls] %ls%ls  %ldx%ld @ %.0f Hz at (%ld,%ld)  %ls\r\n", m.active ? L"on " : L"off", m.label.c_str(),
                m.primary ? L" (main)" : L"", m.rect.right - m.rect.left, m.rect.bottom - m.rect.top, m.hz, m.rect.left,
                m.rect.top, TechName(m.tech).c_str());
    s += L"        id: " + m.key + L"\r\n";
  }
  AudioDevice def = audio_.Default(false), defMic = audio_.Default(true);
  s += L"\r\nPlayback devices:\r\n";
  for (auto& d : audio_.List(false)) s += (d.id == def.id ? L"  * " : L"    ") + d.name + L"\r\n";
  s += L"\r\nRecording devices:\r\n";
  for (auto& d : audio_.List(true)) s += (d.id == defMic.id ? L"  * " : L"    ") + d.name + L"\r\n";
  s += L"\r\nProfiles:\r\n";
  for (auto& n : ProfileNames()) {
    const ProfileData& pd = profiles_[Lower(n)];
    s += (IEquals(n, activeProfile_) ? L"  * " : L"    ") + n + L": " + (pd.hasSnap ? pd.snap.Describe() : L"(no display layout)") + L"\r\n";
  }
  s += L"\r\nHotkeys:\r\n";
  for (auto& hk : cfg_.hotkeys) s += L"    " + hk.first + L" -> " + hk.second + L"\r\n";
  for (auto& e : hotkeyErrors_) s += L"    problem: " + e + L"\r\n";
  s += L"\r\nDisplay hooks:\r\n";
  for (auto& c : cfg_.monitors) {
    for (auto& h : c.onTurnOn) s += L"    " + c.match + L" on:  " + h + L"\r\n";
    for (auto& h : c.onTurnOff) s += L"    " + c.match + L" off: " + h + L"\r\n";
  }
  s += L"\r\nSettings: " + ConfigPath() + L"\r\nLog: " + LogPath() + L"\r\n";
  return Narrow(s);
}

void App::Broadcast() {
  if (subs_.empty()) return;
  std::string st = StatusJson(false);
  COPYDATASTRUCT cds{kCdStatus, (DWORD)st.size(), (void*)st.data()};
  for (size_t i = 0; i < subs_.size();) {
    DWORD_PTR res;
    if (!IsWindow(subs_[i]) ||
        !SendMessageTimeoutW(subs_[i], WM_COPYDATA, (WPARAM)wnd_, (LPARAM)&cds, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &res)) {
      if (!IsWindow(subs_[i])) { subs_.erase(subs_.begin() + i); continue; }
    }
    i++;
  }
}

// ---- tray ------------------------------------------------------------------
void App::AddTray() {
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = wnd_;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  wcscpy(nid.szTip, L"DisplayFlip");
  if (Shell_NotifyIconW(NIM_ADD, &nid)) {
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    trayAdded_ = true;
  }
  UpdateTray();
}

void App::UpdateTray() {
  if (!trayAdded_) return;
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = wnd_;
  nid.uID = 1;
  nid.uFlags = NIF_TIP | NIF_SHOWTIP;
  int on = 0;
  for (auto& m : mons_) on += m.active;
  std::wstring tip = L"DisplayFlip\n" + (activeProfile_.empty() ? L"" : L"Profile: " + activeProfile_ + L"\n") +
                     Format(L"%d of %u displays on", on, (unsigned)mons_.size());
  wcsncpy(nid.szTip, tip.c_str(), 127);
  nid.szTip[127] = 0;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowMenu() {
  mons_ = QueryMonitors();
  menuProfiles_ = ProfileNames();
  menuMons_ = mons_;
  HMENU m = CreatePopupMenu();
  std::wstring head = L"DisplayFlip" + (activeProfile_.empty() ? L"" : L"  \u2014  " + activeProfile_);
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

  std::map<std::wstring, std::wstring> profileKeys;
  for (auto& p : cfg_.profiles)
    if (!p.hotkey.empty()) profileKeys[Lower(p.name)] = p.hotkey;
  for (auto& hk : cfg_.hotkeys) {
    auto a = SplitArgs(hk.second);
    if (a.size() >= 2 && IEquals(a[0], L"profile")) profileKeys[Lower(a[1])] = hk.first;
  }
  if (menuProfiles_.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No profiles yet");
  for (size_t i = 0; i < menuProfiles_.size(); i++) {
    std::wstring t = menuProfiles_[i];
    auto k = profileKeys.find(Lower(t));
    if (k != profileKeys.end()) t += L"\t" + k->second;
    AppendMenuW(m, MF_STRING | (IEquals(menuProfiles_[i], activeProfile_) ? MF_CHECKED : 0), ID_PROFILE + i, t.c_str());
  }
  AppendMenuW(m, MF_STRING, ID_SAVE_NEW, L"Save current layout as profile\u2026");
  if (!menuProfiles_.empty()) {
    HMENU up = CreatePopupMenu(), del = CreatePopupMenu();
    for (size_t i = 0; i < menuProfiles_.size(); i++) {
      AppendMenuW(up, MF_STRING, ID_UPDATE + i, menuProfiles_[i].c_str());
      AppendMenuW(del, MF_STRING, ID_DELETE + i, menuProfiles_[i].c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)up, L"Update profile with current layout");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)del, L"Delete profile");
  }
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

  std::map<std::wstring, std::wstring> monKeys;
  for (auto& hk : cfg_.hotkeys) {
    auto a = SplitArgs(hk.second);
    if (a.size() >= 2 && (IEquals(a[0], L"monitor") || IEquals(a[0], L"display"))) monKeys[Lower(a[1])] = hk.first;
    if (!a.empty() && IEquals(a[0], L"tv")) monKeys[Lower(DefaultMonitorSpec())] = hk.first;
  }
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Displays (click to turn on/off)");
  HMENU prim = CreatePopupMenu();
  for (size_t i = 0; i < menuMons_.size(); i++) {
    const LiveMonitor& mon = menuMons_[i];
    std::wstring t = mon.label + (mon.primary ? L"  (main)" : L"");
    auto k = monKeys.find(Lower(mon.label));
    if (k == monKeys.end()) k = monKeys.find(Lower(mon.name));
    if (k != monKeys.end()) t += L"\t" + k->second;
    AppendMenuW(m, MF_STRING | (mon.active ? MF_CHECKED : 0), ID_MONITOR + i, t.c_str());
    if (mon.active) AppendMenuW(prim, MF_STRING | (mon.primary ? MF_CHECKED : 0), ID_PRIMARY + i, mon.label.c_str());
  }
  menuAsleep_ = AsleepDisplays();
  for (size_t i = 0; i < menuAsleep_.size(); i++)
    AppendMenuW(m, MF_STRING, ID_WAKE + i, (menuAsleep_[i] + L"  (asleep - click to wake)").c_str());
  AppendMenuW(m, MF_POPUP, (UINT_PTR)prim, L"Main display");
  AppendMenuW(m, MF_STRING | (lastPlan_.empty() ? MF_GRAYED : 0), ID_RESTORE, L"Put windows back");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings\u2026");
  AppendMenuW(m, MF_STRING, ID_FOLDER, L"Open data folder");
  AppendMenuW(m, MF_STRING, ID_LOG, L"Open log");
  AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
  if (FileExists(L"C:\\Program Files\\Elgato\\StreamDeck\\StreamDeck.exe"))
    AppendMenuW(m, MF_STRING, ID_STREAMDECK, L"Install / update Stream Deck plugin");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(wnd_);
  TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, wnd_, nullptr);
  PostMessageW(wnd_, WM_NULL, 0, 0);
  DestroyMenu(m);
}

void App::OnMenu(UINT id) {
  if (id >= ID_WAKE && id < ID_WAKE + menuAsleep_.size()) {
    Exec(L"monitor " + QuoteArg(menuAsleep_[id - ID_WAKE]) + L" on", nullptr);
  } else if (id >= ID_PRIMARY && id < ID_PRIMARY + menuMons_.size()) {
    Exec(L"primary " + QuoteArg(menuMons_[id - ID_PRIMARY].label), nullptr);
  } else if (id >= ID_DELETE && id < ID_DELETE + menuProfiles_.size()) {
    Exec(L"delete " + QuoteArg(menuProfiles_[id - ID_DELETE]), nullptr);
  } else if (id >= ID_UPDATE && id < ID_UPDATE + menuProfiles_.size()) {
    SaveProfile(menuProfiles_[id - ID_UPDATE], false);
  } else if (id >= ID_MONITOR && id < ID_MONITOR + menuMons_.size()) {
    Exec(L"monitor " + QuoteArg(menuMons_[id - ID_MONITOR].label) + L" toggle", nullptr);
  } else if (id >= ID_PROFILE && id < ID_PROFILE + menuProfiles_.size()) {
    Exec(L"profile " + QuoteArg(menuProfiles_[id - ID_PROFILE]), nullptr);
  } else {
    switch (id) {
      case ID_SAVE_NEW: SaveProfile(L"", true); break;
      case ID_SETTINGS: ShellOpen(ConfigPath()); break;
      case ID_FOLDER: ShellOpen(DataDir()); break;
      case ID_LOG: ShellOpen(LogPath()); break;
      case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
      case ID_RESTORE: RestoreNow(); break;
      case ID_STREAMDECK: Exec(L"streamdeck", nullptr); break;
      case ID_EXIT: DestroyWindow(wnd_); break;
    }
  }
}

// Last line of defence: a real bug in our own code (or a jump into nowhere) logs
// itself and relaunches the app, so the hotkey keeps working. Faults inside
// system DLLs are left alone: Windows handles plenty of those internally.
ULONGLONG g_startedAt = 0;
LONG CALLBACK CrashGuard(EXCEPTION_POINTERS* ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
    return EXCEPTION_CONTINUE_SEARCH;
  void* addr = ep->ExceptionRecord->ExceptionAddress;
  HMODULE mod = nullptr;
  bool known = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)addr, &mod);
  if (known && mod != GetModuleHandleW(nullptr)) return EXCEPTION_CONTINUE_SEARCH;
  Log(L"CRASH 0x%08lx at %p (offset 0x%llx) - restarting", code, addr,
      known ? (unsigned long long)((char*)addr - (char*)mod) : 0ULL);
  if (NowMs() - g_startedAt > 15000) LaunchDetached(QuoteArg(ExePath()) + L" --background", false);
  TerminateProcess(GetCurrentProcess(), 3);
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

int RunApp(HINSTANCE inst, const std::wstring& firstCommand) {
  g_startedAt = NowMs();
  AddVectoredExceptionHandler(1, CrashGuard);
  App app;
  g_app = &app;
  int r = app.Run(inst, firstCommand);
  g_app = nullptr;
  return r;
}

// ---- client side -----------------------------------------------------------
HWND FindApp() { return FindWindowW(kMainClass, nullptr); }

bool SendToApp(HWND app, const std::wstring& command, HWND replyTo, DWORD timeoutMs) {
  std::string u = Narrow(command);
  COPYDATASTRUCT cds{kCdCommand, (DWORD)u.size(), (void*)u.data()};
  DWORD_PTR res = 0;
  return SendMessageTimeoutW(app, WM_COPYDATA, (WPARAM)replyTo, (LPARAM)&cds, SMTO_ABORTIFHUNG, timeoutMs, &res) != 0;
}

namespace {
std::string* g_reply = nullptr;
LRESULT CALLBACK ReplyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_COPYDATA) {
    auto* cds = (COPYDATASTRUCT*)l;
    if (cds->dwData == kCdReply && g_reply) g_reply->assign((const char*)cds->lpData, cds->cbData);
    return TRUE;
  }
  return DefWindowProcW(h, m, w, l);
}
}  // namespace

std::string QueryApp(HWND app, const std::wstring& command, DWORD timeoutMs) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = ReplyProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"DisplayFlip.Reply";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  ChangeWindowMessageFilterEx(w, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
  std::string reply;
  g_reply = &reply;
  SendToApp(app, command, w, timeoutMs);
  g_reply = nullptr;
  DestroyWindow(w);
  return reply;
}

}  // namespace df
