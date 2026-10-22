#include "winlayout.h"
#include <dwmapi.h>

namespace df {

namespace {

inline LONG W(const RECT& r) { return r.right - r.left; }
inline LONG H(const RECT& r) { return r.bottom - r.top; }

bool RectNear(const RECT& a, const RECT& b, LONG tol) {
  return std::abs(a.left - b.left) <= tol && std::abs(a.top - b.top) <= tol && std::abs(a.right - b.right) <= tol &&
         std::abs(a.bottom - b.bottom) <= tol;
}

// Same spot relative to another monitor; size kept (shrunk only if it does not fit).
RECT MapRect(const RECT& r, const RECT& from, const RECT& to) {
  LONG w = W(r), h = H(r);
  LONG dx = r.left - from.left, dy = r.top - from.top;
  if ((W(from) != W(to) || H(from) != H(to)) && W(from) > 0 && H(from) > 0) {
    dx = (LONG)((long long)dx * W(to) / W(from));
    dy = (LONG)((long long)dy * H(to) / H(from));
  }
  w = std::min(w, W(to));
  h = std::min(h, H(to));
  RECT o{to.left + dx, to.top + dy, to.left + dx + w, to.top + dy + h};
  if (o.right > to.right) OffsetRect(&o, to.right - o.right, 0);
  if (o.bottom > to.bottom) OffsetRect(&o, 0, to.bottom - o.bottom);
  if (o.left < to.left) OffsetRect(&o, to.left - o.left, 0);
  if (o.top < to.top) OffsetRect(&o, 0, to.top - o.top);
  return o;
}

bool IsTool(HWND h) { return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0; }

// WINDOWPLACEMENT uses "workspace" coordinates: screen coordinates shifted by
// the taskbar of the window's monitor (zero for a bottom taskbar).
POINT WorkspaceOffset(const MonMap& mm, const RECT& r) {
  const MonMap::M* m = mm.FromRect(r);
  if (!m) return POINT{0, 0};
  return POINT{m->work.left - m->rect.left, m->work.top - m->rect.top};
}

void MoveTo(HWND h, const WinRec& cur, WinState st, const RECT& r, const MonMap::M& mon, bool restoreToMax) {
  RECT wsr = r;
  if (!IsTool(h)) OffsetRect(&wsr, -(mon.work.left - mon.rect.left), -(mon.work.top - mon.rect.top));
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  GetWindowPlacement(h, &wp);
  wp.flags = WPF_ASYNCWINDOWPLACEMENT;
  wp.rcNormalPosition = wsr;
  switch (st) {
    case WinState::Normal:
      if (cur.state == WinState::Normal) {
        SetWindowPos(h, nullptr, r.left, r.top, W(r), H(r),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
      } else {
        wp.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &wp);
      }
      break;
    case WinState::Maximized:
      if (cur.state == WinState::Maximized) {
        // restore onto the target monitor, then maximize there
        wp.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &wp);
        ShowWindowAsync(h, SW_SHOWMAXIMIZED);
      } else {
        wp.showCmd = SW_SHOWMAXIMIZED;
        SetWindowPlacement(h, &wp);
      }
      break;
    case WinState::Minimized:
      wp.showCmd = SW_SHOWMINNOACTIVE;
      if (restoreToMax) wp.flags |= WPF_RESTORETOMAXIMIZED;
      SetWindowPlacement(h, &wp);
      break;
  }
}

Json RectJson(const RECT& r) {
  Json a = Json::Arr();
  a.push(Json((long long)r.left));
  a.push(Json((long long)r.top));
  a.push(Json((long long)r.right));
  a.push(Json((long long)r.bottom));
  return a;
}

RECT RectFrom(const Json& a) {
  return RECT{(LONG)a.at(0).asNum(), (LONG)a.at(1).asNum(), (LONG)a.at(2).asNum(), (LONG)a.at(3).asNum()};
}

}  // namespace

// ---- MonMap ----------------------------------------------------------------
MonMap MonMap::Current(const std::vector<LiveMonitor>& live) {
  MonMap mm;
  for (auto& m : live)
    if (m.active) mm.mons.push_back({m.key, m.label, m.rect, m.work, m.primary});
  return mm;
}

const MonMap::M* MonMap::Get(const std::wstring& key) const {
  for (auto& m : mons)
    if (m.key == key) return &m;
  return nullptr;
}

const MonMap::M* MonMap::Primary() const {
  for (auto& m : mons)
    if (m.primary) return &m;
  return mons.empty() ? nullptr : &mons[0];
}

const MonMap::M* MonMap::FromRect(const RECT& r) const {
  const M* best = nullptr;
  long long bestArea = 0;
  for (auto& m : mons) {
    RECT i;
    if (IntersectRect(&i, &r, &m.rect)) {
      long long a = (long long)W(i) * H(i);
      if (a > bestArea) { bestArea = a; best = &m; }
    }
  }
  if (best) return best;
  long long bestD = -1;
  LONG cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  for (auto& m : mons) {
    LONG mx = std::max(m.rect.left, std::min(cx, m.rect.right)), my = std::max(m.rect.top, std::min(cy, m.rect.bottom));
    long long d = (long long)(mx - cx) * (mx - cx) + (long long)(my - cy) * (my - cy);
    if (bestD < 0 || d < bestD) { bestD = d; best = &m; }
  }
  return best;
}

// ---- capture ---------------------------------------------------------------
bool IsManagedWindow(HWND h) {
  if (!IsWindowVisible(h)) return false;
  LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
  LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
  if (style & WS_CHILD) return false;
  if (ex & WS_EX_TRANSPARENT) return false;  // click-through overlays (GeForce overlay, HUDs) manage themselves
  DWORD cloaked = 0;
  // windows on other virtual desktops are shell-cloaked and still count; app-cloaked ones are not real
  if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && (cloaked & ~DWM_CLOAKED_SHELL))
    return false;
  wchar_t cls[128] = {};
  GetClassNameW(h, cls, 128);
  static const wchar_t* skip[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                  L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow",
                                  L"TopLevelWindowForOverflowXamlIsland", L"XamlExplorerHostIslandWindow",
                                  L"Shell_InputSwitchTopLevelWindow", L"MultitaskingViewFrame", L"ForegroundStaging",
                                  L"Xaml_WindowedPopupClass", L"tooltips_class32", L"#32768", L"SysShadow",
                                  L"EdgeUiInputTopWndClass", L"ApplicationManager_ImmersiveShellWindow",
                                  L"DisplayFlip.Main", L"DisplayFlip.Osd", L"DisplayFlip.Prompt"};
  for (auto* s : skip)
    if (!wcscmp(cls, s)) return false;
  wchar_t title[8];
  bool titled = GetWindowTextW(h, title, 8) > 0;
  if (!titled && !(ex & WS_EX_APPWINDOW)) return false;
  RECT r;
  GetWindowRect(h, &r);
  if (W(r) <= 0 || H(r) <= 0) {
    if (!IsIconic(h)) return false;
  }
  if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) {
    // tool windows: only real panels (dashboards), not overlays, popups or bubbles owned by another window
    if (ex & WS_EX_NOACTIVATE) return false;
    if (GetWindow(h, GW_OWNER)) return false;
    if (!IsIconic(h) && (W(r) < 120 || H(r) < 80)) return false;
  }
  return true;
}

bool CaptureWindow(HWND h, const MonMap& mm, WinRec& r) {
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  if (!GetWindowPlacement(h, &wp)) return false;
  RECT wr;
  if (!GetWindowRect(h, &wr)) return false;
  r.hwnd = h;
  GetWindowThreadProcessId(h, &r.pid);
  wchar_t buf[256] = {};
  GetClassNameW(h, buf, 256);
  r.cls = buf;
  buf[0] = 0;
  GetWindowTextW(h, buf, 256);
  r.title = buf;
  RECT normal = wp.rcNormalPosition;
  if (!IsTool(h)) {
    POINT off = WorkspaceOffset(mm, normal);
    OffsetRect(&normal, off.x, off.y);
  }
  const MonMap::M* mon = nullptr;
  if (IsIconic(h)) {
    r.state = WinState::Minimized;
    r.restoreToMax = (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;
    r.normal = normal;
    mon = mm.FromRect(normal);
  } else if (IsZoomed(h)) {
    r.state = WinState::Maximized;
    mon = mm.FromRect(wr);
    const MonMap::M* nmon = mm.FromRect(normal);
    // keep the restored rect on the monitor it is maximized on
    r.normal = (mon && nmon && nmon != mon) ? MapRect(normal, nmon->rect, mon->rect) : normal;
  } else {
    r.state = WinState::Normal;
    r.normal = wr;
    mon = mm.FromRect(wr);
  }
  if (!mon) return false;
  r.monKey = mon->key;
  r.monRect = mon->rect;
  return true;
}

struct EnumCtx {
  const MonMap* mm;
  Layout* out;
  std::map<DWORD, std::wstring>* exes;
};

static BOOL CALLBACK EnumWinProc(HWND h, LPARAM lp) {
  EnumCtx* c = (EnumCtx*)lp;
  if (!IsManagedWindow(h)) return TRUE;
  WinRec r;
  if (!CaptureWindow(h, *c->mm, r)) return TRUE;
  auto it = c->exes->find(r.pid);
  if (it == c->exes->end()) it = c->exes->emplace(r.pid, ProcessImageName(r.pid)).first;
  r.exe = it->second;
  c->out->wins.push_back(std::move(r));
  return TRUE;
}

Layout CaptureLayout(const MonMap& mm) {
  Layout l;
  std::map<DWORD, std::wstring> exes;
  EnumCtx c{&mm, &l, &exes};
  EnumWindows(EnumWinProc, (LPARAM)&c);
  l.foreground = GetForegroundWindow();
  return l;
}

const WinRec* Layout::Find(HWND h) const {
  for (auto& w : wins)
    if (w.hwnd == h) return &w;
  return nullptr;
}

Json WinRec::ToJson() const {
  Json j = Json::Obj();
  j.set("hwnd", Json((unsigned long long)(uintptr_t)hwnd));
  j.set("pid", Json((unsigned)pid));
  j.set("exe", Json(exe));
  j.set("class", Json(cls));
  j.set("title", Json(title));
  j.set("monitor", Json(monKey));
  j.set("normal", RectJson(normal));
  j.set("monRect", RectJson(monRect));
  j.set("state", Json((int)state));
  j.set("restoreToMax", Json(restoreToMax));
  return j;
}

WinRec WinRec::FromJson(const Json& j) {
  WinRec r;
  r.hwnd = (HWND)(uintptr_t)(unsigned long long)j.num("hwnd");
  r.pid = (DWORD)j.num("pid");
  r.exe = j.str("exe");
  r.cls = j.str("class");
  r.title = j.str("title");
  r.monKey = j.str("monitor");
  r.normal = RectFrom(j["normal"]);
  r.monRect = RectFrom(j["monRect"]);
  r.state = (WinState)(int)j.num("state");
  r.restoreToMax = j.boolean("restoreToMax");
  return r;
}

Json Layout::ToJson() const {
  Json a = Json::Arr();
  for (auto& w : wins) a.push(w.ToJson());
  return a;
}

Layout Layout::FromJson(const Json& j) {
  Layout l;
  for (size_t i = 0; i < j.size(); i++) l.wins.push_back(WinRec::FromJson(j.at(i)));
  return l;
}

// ---- restore ---------------------------------------------------------------
static bool StillSameWindow(const WinRec& r) {
  if (!r.hwnd || !IsWindow(r.hwnd)) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(r.hwnd, &pid);
  if (pid != r.pid) return false;
  wchar_t cls[256] = {};
  GetClassNameW(r.hwnd, cls, 256);
  return r.cls.empty() || r.cls == cls;
}

std::vector<WinTarget> PlanRestore(const Layout& before, const Layout* memory, const std::set<std::wstring>& oldMons,
                                   const MonMap& now, const std::wstring& orphans, bool planned) {
  std::vector<WinTarget> out;
  std::set<HWND> seen;
  auto consider = [&](HWND h) {
    if (!seen.insert(h).second) return;
    const WinRec* s = before.Find(h);
    const WinRec* m = memory ? memory->Find(h) : nullptr;
    if (s && !StillSameWindow(*s)) return;
    if (m && !StillSameWindow(*m)) m = nullptr;
    if (!s && !m) return;
    bool displaced = s && !now.Has(s->monKey);
    bool returning = m && now.Has(m->monKey) && !oldMons.count(m->monKey);
    if (m && now.Has(m->monKey) && (displaced || returning)) {
      out.push_back({h, *m, false});  // back to where it was the last time this layout was active
      return;
    }
    if (!s) return;
    if (displaced) {
      if (!planned || orphans == L"windows") return;  // let Windows place it
      const MonMap::M* prim = now.Primary();
      if (!prim) return;
      WinRec w = *s;
      w.normal = MapRect(s->normal, s->monRect, prim->rect);
      w.monKey = prim->key;
      w.monRect = prim->rect;
      if (orphans == L"minimize") {
        w.restoreToMax = s->state == WinState::Maximized || (s->state == WinState::Minimized && s->restoreToMax);
        w.state = WinState::Minimized;
      }
      out.push_back({h, w, false});
      return;
    }
    if (planned) out.push_back({h, *s, true});  // stays where it was; undo any shuffling Windows does
  };
  for (auto& w : before.wins) consider(w.hwnd);
  if (memory)
    for (auto& w : memory->wins) consider(w.hwnd);
  return out;
}

int ApplyTargets(std::vector<WinTarget>& targets, const MonMap& now) {
  int moved = 0;
  for (auto& t : targets) {
    if (!t.hwnd) continue;
    if (!IsWindow(t.hwnd)) { t.hwnd = nullptr; continue; }
    const MonMap::M* mon = now.Get(t.want.monKey);
    if (!mon) continue;
    RECT want = MapRect(t.want.normal, t.want.monRect, mon->rect);
    WinRec cur;
    if (!CaptureWindow(t.hwnd, now, cur)) continue;
    bool same = cur.state == t.want.state && cur.monKey == t.want.monKey;
    // minimized windows too: their restored rect decides where they reappear
    if (same && cur.state != WinState::Maximized) same = RectNear(cur.normal, want, 2);
    if (same) continue;
    MoveTo(t.hwnd, cur, t.want.state, want, *mon, t.want.restoreToMax);
    moved++;
  }
  return moved;
}

bool PlaceWindow(HWND h, const MonMap& mm, const std::wstring& monKey, const std::wstring& state, const RECT* rectInWork) {
  const MonMap::M* mon = mm.Get(monKey);
  if (!mon) return false;
  WinRec cur;
  if (!CaptureWindow(h, mm, cur)) return false;
  const MonMap::M* from = mm.Get(cur.monKey);
  WinState st = WinState::Normal;
  if (IEquals(state, L"maximized") || IEquals(state, L"maximize")) st = WinState::Maximized;
  else if (IEquals(state, L"minimized") || IEquals(state, L"minimize")) st = WinState::Minimized;
  RECT target;
  if (rectInWork) {
    target = RECT{mon->work.left + rectInWork->left, mon->work.top + rectInWork->top, mon->work.left + rectInWork->right,
                  mon->work.top + rectInWork->bottom};
  } else if (IEquals(state, L"fullscreen")) {
    target = mon->rect;  // cover the whole screen (borderless windows)
  } else if (IEquals(state, L"fill")) {
    target = mon->work;
  } else {
    target = MapRect(cur.normal, from ? from->rect : cur.monRect, mon->rect);
  }
  if (cur.state == st && cur.monKey == monKey && (st != WinState::Normal || RectNear(cur.normal, target, 2))) return false;
  MoveTo(h, cur, st, target, *mon, false);
  return true;
}

}  // namespace df
