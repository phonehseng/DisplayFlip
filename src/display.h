// Display topology: query, snapshot and apply through the Windows CCD API
// (QueryDisplayConfig / SetDisplayConfig), the same API Windows Settings uses.
#pragma once
#include "common.h"
#include "json.h"

namespace df {

// One active path (monitor) of a display layout, stored in a portable way:
// monitors are identified by their EDID device path, never by adapter LUID
// (LUIDs change on every boot / driver update).
struct PathSnap {
  std::wstring key;          // monitor device path, lower case (stable id)
  std::wstring name;         // EDID friendly name, e.g. "Philips GA4"
  std::wstring adapterPath;  // GPU device path
  UINT32 sourceId = 0, targetId = 0;
  UINT32 edidMfg = 0, edidProduct = 0, connector = 0;
  // source mode (desktop)
  UINT32 width = 0, height = 0, pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
  LONG x = 0, y = 0;
  // target mode (signal)
  bool hasTarget = false;
  DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal{};
  UINT32 rotation = DISPLAYCONFIG_ROTATION_IDENTITY, scaling = DISPLAYCONFIG_SCALING_IDENTITY;
  UINT32 scanline = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE, tech = 0, pathFlags = 0;
  DISPLAYCONFIG_RATIONAL refresh{0, 0};

  RECT Rect() const { return RECT{x, y, x + (LONG)width, y + (LONG)height}; }
  double Hz() const;
  Json ToJson() const;
  static PathSnap FromJson(const Json& j);
};

struct DisplaySnap {
  std::vector<PathSnap> paths;
  Json ToJson() const;
  static DisplaySnap FromJson(const Json& j);
  const PathSnap* Find(const std::wstring& key) const;
  std::wstring Describe() const;
};

// A connected monitor (active or not).
struct LiveMonitor {
  std::wstring key, name, label;  // label = name, or "name #2" when several share a name
  std::wstring gdi;               // \\.\DISPLAYn when active
  LUID adapter{};
  UINT32 targetId = 0, tech = 0;
  bool active = false, primary = false;
  RECT rect{}, work{};
  double hz = 0;
};

std::vector<LiveMonitor> QueryMonitors();
bool CaptureSnap(DisplaySnap& out);
std::wstring TopologyKey(const std::vector<LiveMonitor>& mons);   // sorted keys of active monitors

// Resolve a user supplied monitor reference: label, EDID name, part of the
// device path (e.g. "UID4352"), or "primary".
const LiveMonitor* FindMonitor(const std::vector<LiveMonitor>& mons, const std::wstring& spec);

// True when every connected monitor of `want` is active at the same place/size
// and nothing else is active.
bool SnapMatchesCurrent(const DisplaySnap& want, const DisplaySnap& cur, const std::vector<LiveMonitor>& mons);

// Apply a layout. Tries an exact apply first, then progressively lets Windows
// fill in modes from its own database. Returns false with a message on failure.
bool ApplySnap(const DisplaySnap& snap, std::wstring* err, std::wstring* how = nullptr);

// Turn a monitor off/on in the current layout. `memory` is the monitor's last
// known placement (may be null for a monitor never seen active).
bool DisableMonitor(const std::wstring& key, const std::wstring& preferredPrimary, std::wstring* err);
bool EnableMonitor(const std::wstring& key, const PathSnap* memory, std::wstring* err);
bool SetPrimaryMonitor(const std::wstring& key, std::wstring* err);

}  // namespace df
