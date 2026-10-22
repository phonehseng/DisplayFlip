// Stream Deck plugin mode. Stream Deck starts "DisplayFlip.exe -port N -pluginUUID ...";
// we talk to it over its local WebSocket and forward key presses to the tray app.
// Buttons show live state (display on/off, profile active) pushed from the tray app.
#include "app.h"
#include "json.h"
#include <objbase.h>
#include <winhttp.h>

namespace df {

namespace {

const UINT WM_SD_MSG = WM_APP + 10;  // lParam = std::string* (owned by receiver)
const UINT WM_SD_CLOSED = WM_APP + 11;
const UINT_PTR T_CHECK = 1;
const std::string kActMonitor = "com.displayflip.monitor";
const std::string kActProfile = "com.displayflip.profile";
const std::string kActToggle = "com.displayflip.toggle";
const std::string kActCommand = "com.displayflip.command";

struct Ctx {
  std::string action;
  Json settings;
  int state = -1;
};

class Plugin {
 public:
  int Run(HINSTANCE inst, int port, const std::wstring& uuid, const std::wstring& regEvent);
  LRESULT Proc(HWND h, UINT m, WPARAM w, LPARAM l);

 private:
  bool Connect(int port);
  void Send(const Json& j);
  void OnEvent(const Json& j);
  void UpdateAll(bool force);
  void Update(const std::string& context, Ctx& c, bool force);
  std::wstring CommandFor(const Ctx& c);
  HWND EnsureApp();
  void Subscribe();
  void SendLists(const std::string& action, const std::string& context);
  static DWORD WINAPI RecvThread(LPVOID p);

  HWND wnd_ = nullptr, app_ = nullptr;
  HINTERNET ses_ = nullptr, con_ = nullptr, ws_ = nullptr;
  std::map<std::string, Ctx> ctx_;
  Json status_;
  std::string lastReply_;
};

Plugin* g_plugin = nullptr;

LRESULT CALLBACK PluginProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (g_plugin) return g_plugin->Proc(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}

bool Plugin::Connect(int port) {
  ses_ = WinHttpOpen(L"DisplayFlip", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses_) return false;
  con_ = WinHttpConnect(ses_, L"127.0.0.1", (INTERNET_PORT)port, 0);
  if (!con_) return false;
  HINTERNET req = WinHttpOpenRequest(con_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!req) return false;
  WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
  bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr);
  if (ok) ws_ = WinHttpWebSocketCompleteUpgrade(req, 0);
  WinHttpCloseHandle(req);
  return ws_ != nullptr;
}

void Plugin::Send(const Json& j) {
  if (!ws_) return;
  std::string s = j.Dump(false);
  WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)s.data(), (DWORD)s.size());
}

DWORD WINAPI Plugin::RecvThread(LPVOID p) {
  Plugin* self = (Plugin*)p;
  std::string msg;
  std::vector<BYTE> buf(64 * 1024);
  for (;;) {
    DWORD read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    if (WinHttpWebSocketReceive(self->ws_, buf.data(), (DWORD)buf.size(), &read, &type) != NO_ERROR) break;
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
    msg.append((const char*)buf.data(), read);
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
      PostMessageW(self->wnd_, WM_SD_MSG, 0, (LPARAM) new std::string(std::move(msg)));
      msg.clear();
    }
  }
  PostMessageW(self->wnd_, WM_SD_CLOSED, 0, 0);
  return 0;
}

HWND Plugin::EnsureApp() {
  HWND a = FindApp();
  if (!a) {
    std::wstring exe = RegisteredExe();
    if (exe.empty() || !FileExists(exe)) exe = ExePath();
    Log(L"streamdeck: starting %ls", exe.c_str());
    LaunchDetached(QuoteArg(exe) + L" --background", false);
    for (int i = 0; i < 100 && !a; i++) {
      Sleep(50);
      a = FindApp();
    }
  }
  if (a != app_) {
    app_ = a;
    if (a) Subscribe();
  }
  return a;
}

void Plugin::Subscribe() {
  if (app_) SendToApp(app_, L"subscribe", wnd_, 3000);  // the reply carries the first status
}

std::wstring Plugin::CommandFor(const Ctx& c) {
  const Json& s = c.settings;
  if (c.action == kActMonitor) {
    std::wstring mon = s.str("monitor"), mode = s.str("mode", L"toggle");
    if (mode.empty()) mode = L"toggle";
    return mon.empty() ? L"tv " + mode : L"monitor " + QuoteArg(mon) + L" " + mode;
  }
  if (c.action == kActProfile) return L"profile " + QuoteArg(s.str("profile"));
  if (c.action == kActToggle) return L"toggle " + QuoteArg(s.str("a")) + L" " + QuoteArg(s.str("b"));
  if (c.action == kActCommand) return s.str("command");
  return L"";
}

void Plugin::Update(const std::string& context, Ctx& c, bool force) {
  if (!status_.isObj()) return;
  int st = 0;
  if (c.action == kActMonitor) {
    std::wstring want = c.settings.str("monitor");
    if (want.empty()) want = status_.str("defaultMonitor");
    const Json& ms = status_["monitors"];
    for (size_t i = 0; i < ms.size(); i++) {
      const Json& m = ms.at(i);
      if (IEquals(m.str("label"), want) || IEquals(m.str("name"), want)) { st = m.boolean("active") ? 1 : 0; break; }
    }
  } else if (c.action == kActProfile) {
    st = IEquals(status_.str("profile"), c.settings.str("profile")) ? 1 : 0;
  } else if (c.action == kActToggle) {
    st = IEquals(status_.str("profile"), c.settings.str("b")) ? 1 : 0;
  } else {
    return;
  }
  if (st == c.state && !force) return;
  c.state = st;
  Json p = Json::Obj();
  p.set("state", Json(st));
  Json j = Json::Obj();
  j.set("event", Json("setState"));
  j.set("context", Json(context));
  j.set("payload", p);
  Send(j);
}

void Plugin::UpdateAll(bool force) {
  for (auto& kv : ctx_) Update(kv.first, kv.second, force);
}

void Plugin::SendLists(const std::string& action, const std::string& context) {
  Json p = Json::Obj();
  Json mons = Json::Arr(), profs = Json::Arr();
  const Json& ms = status_["monitors"];
  for (size_t i = 0; i < ms.size(); i++) mons.push(Json(ms.at(i).str("label")));
  const Json& ps = status_["profiles"];
  for (size_t i = 0; i < ps.size(); i++) profs.push(ps.at(i));
  p.set("monitors", mons);
  p.set("profiles", profs);
  p.set("defaultMonitor", status_["defaultMonitor"]);
  p.set("connected", Json(app_ != nullptr));
  Json j = Json::Obj();
  j.set("event", Json("sendToPropertyInspector"));
  j.set("action", Json(action));
  j.set("context", Json(context));
  j.set("payload", p);
  Send(j);
}

void Plugin::OnEvent(const Json& j) {
  std::string ev = j["event"].asStr(), action = j["action"].asStr(), context = j["context"].asStr();
  const Json& payload = j["payload"];
  if (ev == "willAppear" || ev == "didReceiveSettings") {
    Ctx& c = ctx_[context];
    c.action = action;
    c.settings = payload["settings"];
    Update(context, c, true);
  } else if (ev == "willDisappear") {
    ctx_.erase(context);
  } else if (ev == "keyDown") {
    Ctx& c = ctx_[context];
    c.action = action;
    if (payload["settings"].isObj()) c.settings = payload["settings"];
    std::wstring cmd = CommandFor(c);
    Json alert = Json::Obj();
    alert.set("context", Json(context));
    HWND a = cmd.empty() ? nullptr : EnsureApp();
    lastReply_.clear();
    if (!a || !SendToApp(a, cmd, wnd_, 10000) || lastReply_.rfind("error", 0) == 0) {
      alert.set("event", Json("showAlert"));
      Send(alert);
    } else if (action == kActCommand) {
      alert.set("event", Json("showOk"));
      Send(alert);
    }
    Update(context, c, true);  // re-assert our state (Stream Deck may have flipped it)
  } else if (ev == "propertyInspectorDidAppear") {
    EnsureApp();
    SendLists(action, context);
  } else if (ev == "sendToPlugin") {
    SendLists(action, context);
  } else if (ev == "systemDidWakeUp" || ev == "deviceDidConnect") {
    EnsureApp();
    UpdateAll(true);
  }
}

LRESULT Plugin::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_SD_MSG: {
      std::unique_ptr<std::string> s((std::string*)l);
      OnEvent(Json::Parse(*s));
      return 0;
    }
    case WM_SD_CLOSED:
      Log(L"streamdeck: connection closed");
      PostQuitMessage(0);
      return 0;
    case WM_COPYDATA: {
      auto* cds = (COPYDATASTRUCT*)l;
      std::string s((const char*)cds->lpData, cds->cbData);
      if (cds->dwData == kCdStatus || (cds->dwData == kCdReply && !s.empty() && s[0] == '{')) {
        status_ = Json::Parse(s);
        UpdateAll(false);
      }
      if (cds->dwData == kCdReply) lastReply_ = s;
      return TRUE;
    }
    case WM_TIMER:
      if (w == T_CHECK) {
        HWND a = FindApp();
        if (a != app_) {
          app_ = a;
          if (a) Subscribe();
        }
      }
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int Plugin::Run(HINSTANCE inst, int port, const std::wstring& uuid, const std::wstring& regEvent) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = PluginProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"DisplayFlip.SDPlugin";
  RegisterClassW(&wc);
  wnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
  ChangeWindowMessageFilterEx(wnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
  if (!Connect(port)) {
    Log(L"streamdeck: could not connect to port %d", port);
    return 1;
  }
  Json reg = Json::Obj();
  reg.set("event", Json(regEvent));
  reg.set("uuid", Json(uuid));
  Send(reg);
  Log(L"streamdeck: plugin connected");
  CloseHandle(CreateThread(nullptr, 0, RecvThread, this, 0, nullptr));
  EnsureApp();
  SetTimer(wnd_, T_CHECK, 3000, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

}  // namespace

int RunStreamDeckPlugin(HINSTANCE inst, int argc, wchar_t** argv) {
  int port = 0;
  std::wstring uuid, regEvent;
  for (int i = 1; i + 1 < argc; i++) {
    if (!wcscmp(argv[i], L"-port")) port = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"-pluginUUID")) uuid = argv[++i];
    else if (!wcscmp(argv[i], L"-registerEvent")) regEvent = argv[++i];
  }
  if (!port || uuid.empty() || regEvent.empty()) return 2;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Plugin p;
  g_plugin = &p;
  int r = p.Run(inst, port, uuid, regEvent);
  g_plugin = nullptr;
  return r;
}

}  // namespace df
