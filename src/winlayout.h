// Window layout memory: capture where every app window is, and put windows
// back (per monitor, relative to that monitor) after the display layout changes.
#pragma once
#include "common.h"
#include "json.h"
#include "display.h"

namespace df {

enum class WinState { Normal, Maximized, Minimized };

struct WinRec {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  std::wstring cls, title, exe;
  std::wstring monKey;      // monitor the window lives on (its restored rect for minimized windows)
  RECT normal{};            // restored rect, screen coordinates
  RECT monRect{};           // that monitor's rect at capture time
  WinState state = WinState::Normal;
  bool restoreToMax = false;  // minimized window that was maximized before

  Json ToJson() const;
  static WinRec FromJson(const Json& j);
};

struct Layout {
  std::vector<WinRec> wins;
  HWND foreground = nullptr;
  const WinRec* Find(HWND h) const;
  Json ToJson() const;
  static Layout FromJson(const Json& j);
};

// Monitor rects keyed by monitor key, for the current desktop.
struct MonMap {
  struct M { std::wstring key, label; RECT rect, work; bool primary; };
  std::vector<M> mons;
  static MonMap Current(const std::vector<LiveMonitor>& live);
  const M* Get(const std::wstring& key) const;
  const M* Primary() const;
  const M* FromRect(const RECT& r) const;  // monitor with the largest overlap (or nearest)
  bool Has(const std::wstring& key) const { return Get(key) != nullptr; }
};

bool IsManagedWindow(HWND h);
Layout CaptureLayout(const MonMap& mm);
bool CaptureWindow(HWND h, const MonMap& mm, WinRec& out);

struct WinTarget {
  HWND hwnd = nullptr;
  WinRec want;             // monKey + normal rect (relative to want.monRect) + state
  bool protect = false;    // "keep it where it was" rather than "bring it back"
};

// Compute where windows should go after a topology change.
//   before  : layout captured just before the change (old topology)
//   memory  : layout last seen in the new topology (may be empty = fresh session)
//   orphans : "minimize" | "move" | "windows" for windows whose monitor disappeared
std::vector<WinTarget> PlanRestore(const Layout& before, const Layout* memory, const std::set<std::wstring>& oldMons,
                                   const MonMap& now, const std::wstring& orphans, bool planned);

// One restore pass. Returns how many windows were moved this pass.
int ApplyTargets(std::vector<WinTarget>& targets, const MonMap& now);

// Move one window to a monitor (used by window rules).
bool PlaceWindow(HWND h, const MonMap& mm, const std::wstring& monKey, const std::wstring& state, const RECT* rectInWork);

}  // namespace df
