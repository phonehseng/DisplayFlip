#include "display.h"
#include <cmath>

namespace df {

namespace {

bool QueryConfig(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
  for (int tries = 0; tries < 6; tries++) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS) return false;
    paths.assign(np, DISPLAYCONFIG_PATH_INFO{});
    modes.assign(nm, DISPLAYCONFIG_MODE_INFO{});
    LONG r = QueryDisplayConfig(flags, &np, paths.data(), &nm, modes.data(), nullptr);
    if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // topology changed between the two calls
    if (r != ERROR_SUCCESS) return false;
    paths.resize(np);
    modes.resize(nm);
    return true;
  }
  return false;
}

inline bool LuidEq(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
inline uint64_t Luid64(const LUID& l) { return ((uint64_t)(uint32_t)l.HighPart << 32) | l.LowPart; }

std::wstring AdapterPath(const LUID& luid) {
  static std::map<uint64_t, std::wstring> cache;
  auto it = cache.find(Luid64(luid));
  if (it != cache.end()) return it->second;
  DISPLAYCONFIG_ADAPTER_NAME an{};
  an.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
  an.header.size = sizeof(an);
  an.header.adapterId = luid;
  std::wstring r;
  if (DisplayConfigGetDeviceInfo(&an.header) == ERROR_SUCCESS) r = Lower(an.adapterDevicePath);
  cache[Luid64(luid)] = r;
  return r;
}

struct TargetName {
  std::wstring key, name;
  UINT32 mfg = 0, prod = 0, connector = 0, tech = 0;
};

TargetName GetTargetName(const LUID& adapter, UINT32 targetId) {
  TargetName t;
  DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
  tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
  tn.header.size = sizeof(tn);
  tn.header.adapterId = adapter;
  tn.header.id = targetId;
  if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
    t.key = Lower(tn.monitorDevicePath);
    t.name = tn.monitorFriendlyDeviceName;
    if (tn.flags.edidIdsValid) {
      t.mfg = tn.edidManufactureId;
      t.prod = tn.edidProductCodeId;
    }
    t.connector = tn.connectorInstance;
    t.tech = (UINT32)tn.outputTechnology;
  }
  if (t.key.empty()) t.key = AdapterPath(adapter) + Format(L"#target%u", targetId);
  if (t.name.empty()) {
    bool internal = t.tech == (UINT32)DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                    t.tech == (UINT32)DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                    t.tech == (UINT32)DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
    t.name = internal ? L"Built-in display" : Format(L"Display %u", targetId);
  }
  return t;
}

std::wstring SourceGdi(const LUID& adapter, UINT32 sourceId) {
  DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
  sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
  sn.header.size = sizeof(sn);
  sn.header.adapterId = adapter;
  sn.header.id = sourceId;
  if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) return {};
  return sn.viewGdiDeviceName;
}

struct GdiMon {
  std::wstring dev;
  RECT mon, work;
};

BOOL CALLBACK EnumMonProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
  MONITORINFOEXW mi;
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(hm, &mi)) ((std::vector<GdiMon>*)lp)->push_back({mi.szDevice, mi.rcMonitor, mi.rcWork});
  return TRUE;
}

PathSnap SnapFromPath(const DISPLAYCONFIG_PATH_INFO& p, const std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
  PathSnap s;
  TargetName tn = GetTargetName(p.targetInfo.adapterId, p.targetInfo.id);
  s.key = tn.key;
  s.name = tn.name;
  s.edidMfg = tn.mfg;
  s.edidProduct = tn.prod;
  s.connector = tn.connector;
  s.adapterPath = AdapterPath(p.targetInfo.adapterId);
  s.sourceId = p.sourceInfo.id;
  s.targetId = p.targetInfo.id;
  UINT32 si = p.sourceInfo.modeInfoIdx;
  if (si < modes.size() && modes[si].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
    const auto& sm = modes[si].sourceMode;
    s.width = sm.width;
    s.height = sm.height;
    s.pixelFormat = (UINT32)sm.pixelFormat;
    s.x = sm.position.x;
    s.y = sm.position.y;
  }
  UINT32 ti = p.targetInfo.modeInfoIdx;
  if (ti < modes.size() && modes[ti].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
    s.hasTarget = true;
    s.signal = modes[ti].targetMode.targetVideoSignalInfo;
  }
  s.rotation = (UINT32)p.targetInfo.rotation;
  s.scaling = (UINT32)p.targetInfo.scaling;
  s.refresh = p.targetInfo.refreshRate;
  s.scanline = (UINT32)p.targetInfo.scanLineOrdering;
  s.tech = (UINT32)p.targetInfo.outputTechnology;
  s.pathFlags = p.flags & ~(UINT32)DISPLAYCONFIG_PATH_ACTIVE;
  return s;
}

LONG TrySet(const wchar_t* label, std::vector<DISPLAYCONFIG_PATH_INFO> paths, std::vector<DISPLAYCONFIG_MODE_INFO> modes,
            UINT32 flags) {
  ULONGLONG t = NowMs();
  LONG r = SetDisplayConfig((UINT32)paths.size(), paths.data(), (UINT32)modes.size(), modes.empty() ? nullptr : modes.data(),
                            flags);
  Log(L"SetDisplayConfig[%ls] paths=%u modes=%u -> %ld (%llu ms)", label, (unsigned)paths.size(), (unsigned)modes.size(), r,
      NowMs() - t);
  return r;
}

}  // namespace

// ---- PathSnap / DisplaySnap serialisation ----------------------------------
double PathSnap::Hz() const {
  if (refresh.Denominator) return (double)refresh.Numerator / refresh.Denominator;
  if (hasTarget && signal.vSyncFreq.Denominator) return (double)signal.vSyncFreq.Numerator / signal.vSyncFreq.Denominator;
  return 0;
}

Json PathSnap::ToJson() const {
  Json j = Json::Obj();
  j.set("name", Json(name));
  j.set("key", Json(key));
  j.set("adapter", Json(adapterPath));
  j.set("sourceId", Json(sourceId));
  j.set("targetId", Json(targetId));
  j.set("edidMfg", Json(edidMfg));
  j.set("edidProduct", Json(edidProduct));
  j.set("connector", Json(connector));
  j.set("x", Json((long long)x));
  j.set("y", Json((long long)y));
  j.set("width", Json(width));
  j.set("height", Json(height));
  j.set("pixelFormat", Json(pixelFormat));
  j.set("rotation", Json(rotation));
  j.set("scaling", Json(scaling));
  j.set("refreshNum", Json(refresh.Numerator));
  j.set("refreshDen", Json(refresh.Denominator));
  j.set("scanline", Json(scanline));
  j.set("tech", Json(tech));
  j.set("pathFlags", Json(pathFlags));
  if (hasTarget) {
    Json s = Json::Obj();
    s.set("pixelRate", Json((unsigned long long)signal.pixelRate));
    s.set("hNum", Json(signal.hSyncFreq.Numerator));
    s.set("hDen", Json(signal.hSyncFreq.Denominator));
    s.set("vNum", Json(signal.vSyncFreq.Numerator));
    s.set("vDen", Json(signal.vSyncFreq.Denominator));
    s.set("activeW", Json(signal.activeSize.cx));
    s.set("activeH", Json(signal.activeSize.cy));
    s.set("totalW", Json(signal.totalSize.cx));
    s.set("totalH", Json(signal.totalSize.cy));
    s.set("videoStandard", Json(signal.videoStandard));
    s.set("scanline", Json((UINT32)signal.scanLineOrdering));
    j.set("signal", s);
  }
  return j;
}

PathSnap PathSnap::FromJson(const Json& j) {
  PathSnap s;
  s.name = j.str("name");
  s.key = Lower(j.str("key"));
  s.adapterPath = Lower(j.str("adapter"));
  s.sourceId = (UINT32)j.num("sourceId");
  s.targetId = (UINT32)j.num("targetId");
  s.edidMfg = (UINT32)j.num("edidMfg");
  s.edidProduct = (UINT32)j.num("edidProduct");
  s.connector = (UINT32)j.num("connector");
  s.x = (LONG)j.num("x");
  s.y = (LONG)j.num("y");
  s.width = (UINT32)j.num("width");
  s.height = (UINT32)j.num("height");
  s.pixelFormat = (UINT32)j.num("pixelFormat", DISPLAYCONFIG_PIXELFORMAT_32BPP);
  s.rotation = (UINT32)j.num("rotation", DISPLAYCONFIG_ROTATION_IDENTITY);
  s.scaling = (UINT32)j.num("scaling", DISPLAYCONFIG_SCALING_IDENTITY);
  s.refresh.Numerator = (UINT32)j.num("refreshNum");
  s.refresh.Denominator = (UINT32)j.num("refreshDen");
  s.scanline = (UINT32)j.num("scanline", DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE);
  s.tech = (UINT32)j.num("tech");
  s.pathFlags = (UINT32)j.num("pathFlags");
  const Json& g = j["signal"];
  if (g.isObj()) {
    s.hasTarget = true;
    s.signal.pixelRate = (UINT64)g.num("pixelRate");
    s.signal.hSyncFreq.Numerator = (UINT32)g.num("hNum");
    s.signal.hSyncFreq.Denominator = (UINT32)g.num("hDen");
    s.signal.vSyncFreq.Numerator = (UINT32)g.num("vNum");
    s.signal.vSyncFreq.Denominator = (UINT32)g.num("vDen");
    s.signal.activeSize.cx = (UINT32)g.num("activeW");
    s.signal.activeSize.cy = (UINT32)g.num("activeH");
    s.signal.totalSize.cx = (UINT32)g.num("totalW");
    s.signal.totalSize.cy = (UINT32)g.num("totalH");
    s.signal.videoStandard = (UINT32)g.num("videoStandard");
    s.signal.scanLineOrdering = (DISPLAYCONFIG_SCANLINE_ORDERING)(UINT32)g.num("scanline");
  }
  return s;
}

Json DisplaySnap::ToJson() const {
  Json a = Json::Arr();
  for (auto& p : paths) a.push(p.ToJson());
  Json j = Json::Obj();
  j.set("displays", a);
  return j;
}

DisplaySnap DisplaySnap::FromJson(const Json& j) {
  DisplaySnap s;
  const Json& a = j["displays"];
  for (size_t i = 0; i < a.size(); i++) s.paths.push_back(PathSnap::FromJson(a.at(i)));
  return s;
}

const PathSnap* DisplaySnap::Find(const std::wstring& key) const {
  for (auto& p : paths)
    if (p.key == key) return &p;
  return nullptr;
}

std::wstring DisplaySnap::Describe() const {
  std::wstring r;
  for (auto& p : paths) {
    if (!r.empty()) r += L", ";
    r += Format(L"%ls %ux%u@%.0fHz (%ld,%ld)", p.name.c_str(), p.width, p.height, p.Hz(), p.x, p.y);
  }
  return r;
}

// ---- live state ------------------------------------------------------------
std::vector<LiveMonitor> QueryMonitors() {
  std::vector<LiveMonitor> out;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryConfig(QDC_ALL_PATHS, paths, modes)) return out;
  std::vector<GdiMon> gdis;
  EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)&gdis);

  for (auto& p : paths) {
    if (!p.targetInfo.targetAvailable) continue;
    LiveMonitor* m = nullptr;
    for (auto& x : out)
      if (LuidEq(x.adapter, p.targetInfo.adapterId) && x.targetId == p.targetInfo.id) { m = &x; break; }
    if (!m) {
      TargetName tn = GetTargetName(p.targetInfo.adapterId, p.targetInfo.id);
      LiveMonitor nm;
      nm.key = tn.key;
      nm.name = tn.name;
      nm.adapter = p.targetInfo.adapterId;
      nm.targetId = p.targetInfo.id;
      nm.tech = tn.tech;
      out.push_back(nm);
      m = &out.back();
    }
    if ((p.flags & DISPLAYCONFIG_PATH_ACTIVE) && !m->active) {
      m->active = true;
      UINT32 si = p.sourceInfo.modeInfoIdx;
      if (si < modes.size() && modes[si].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        const auto& sm = modes[si].sourceMode;
        m->rect = RECT{sm.position.x, sm.position.y, sm.position.x + (LONG)sm.width, sm.position.y + (LONG)sm.height};
      }
      m->work = m->rect;
      m->gdi = SourceGdi(p.sourceInfo.adapterId, p.sourceInfo.id);
      for (auto& g : gdis)
        if (IEquals(g.dev, m->gdi)) { m->rect = g.mon; m->work = g.work; }
      m->primary = m->rect.left == 0 && m->rect.top == 0;
      if (p.targetInfo.refreshRate.Denominator)
        m->hz = (double)p.targetInfo.refreshRate.Numerator / p.targetInfo.refreshRate.Denominator;
    }
  }
  // stable ordering: active by desktop position, then inactive by name
  std::sort(out.begin(), out.end(), [](const LiveMonitor& a, const LiveMonitor& b) {
    if (a.active != b.active) return a.active;
    if (a.active) return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    return Lower(a.name) < Lower(b.name);
  });
  // disambiguate identical monitors (common on triple-screen rigs): "Name #1", "Name #2"...
  for (auto& m : out) {
    std::vector<const LiveMonitor*> same;
    for (auto& o : out)
      if (IEquals(o.name, m.name)) same.push_back(&o);
    if (same.size() <= 1) { m.label = m.name; continue; }
    std::sort(same.begin(), same.end(), [](const LiveMonitor* a, const LiveMonitor* b) { return a->key < b->key; });
    for (size_t i = 0; i < same.size(); i++)
      if (same[i] == &m) m.label = Format(L"%ls #%u", m.name.c_str(), (unsigned)(i + 1));
  }
  return out;
}

bool CaptureSnap(DisplaySnap& out) {
  out.paths.clear();
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryConfig(QDC_ONLY_ACTIVE_PATHS, paths, modes)) return false;
  for (auto& p : paths)
    if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) out.paths.push_back(SnapFromPath(p, modes));
  return !out.paths.empty();
}

std::wstring TopologyKey(const std::vector<LiveMonitor>& mons) {
  std::vector<std::wstring> keys;
  for (auto& m : mons)
    if (m.active) keys.push_back(m.key);
  std::sort(keys.begin(), keys.end());
  std::wstring r;
  for (auto& k : keys) r += k + L"|";
  return r;
}

const LiveMonitor* FindMonitor(const std::vector<LiveMonitor>& mons, const std::wstring& specIn) {
  std::wstring spec = Trim(specIn);
  if (spec.empty()) return nullptr;
  if (IEquals(spec, L"primary")) {
    for (auto& m : mons)
      if (m.primary) return &m;
    return nullptr;
  }
  for (auto& m : mons)
    if (IEquals(m.label, spec)) return &m;
  const LiveMonitor* hit = nullptr;
  int n = 0;
  for (auto& m : mons)
    if (IEquals(m.name, spec)) { hit = &m; n++; }
  if (n >= 1) return hit;  // several identical monitors: first one (use "Name #2" to pick another)
  for (auto& m : mons)
    if (m.key == Lower(spec) || IContains(m.key, spec)) return &m;
  for (auto& m : mons)
    if (IContains(m.label, spec)) return &m;
  return nullptr;
}

bool SnapMatchesCurrent(const DisplaySnap& want, const DisplaySnap& cur, const std::vector<LiveMonitor>& mons) {
  std::set<std::wstring> connected;
  for (auto& m : mons) connected.insert(m.key);
  std::vector<const PathSnap*> w;
  for (auto& p : want.paths)
    if (connected.count(p.key)) w.push_back(&p);
  if (w.empty() || w.size() != cur.paths.size()) return false;
  LONG dx = 0, dy = 0;
  bool first = true;
  for (auto* p : w) {
    const PathSnap* c = cur.Find(p->key);
    if (!c) return false;
    if (c->width != p->width || c->height != p->height) return false;
    if (first) { dx = c->x - p->x; dy = c->y - p->y; first = false; }
    else if (c->x - p->x != dx || c->y - p->y != dy) return false;
    double hw = p->Hz(), hc = c->Hz();
    if (hw > 0 && hc > 0 && std::abs(hw - hc) > 0.6) return false;
  }
  return true;
}

// ---- apply -----------------------------------------------------------------
bool ApplySnap(const DisplaySnap& snap, std::wstring* err, std::wstring* how) {
  std::vector<DISPLAYCONFIG_PATH_INFO> all;
  std::vector<DISPLAYCONFIG_MODE_INFO> allModes;
  if (!QueryConfig(QDC_ALL_PATHS, all, allModes)) {
    if (err) *err = L"Windows refused to report the display configuration";
    return false;
  }

  struct Tgt {
    LUID adapter;
    UINT32 id;
    TargetName tn;
    bool used;
  };
  std::vector<Tgt> tg;
  for (auto& p : all) {
    if (!p.targetInfo.targetAvailable) continue;
    bool seen = false;
    for (auto& t : tg)
      if (LuidEq(t.adapter, p.targetInfo.adapterId) && t.id == p.targetInfo.id) seen = true;
    if (!seen) tg.push_back({p.targetInfo.adapterId, p.targetInfo.id, GetTargetName(p.targetInfo.adapterId, p.targetInfo.id), false});
  }

  struct Res {
    const PathSnap* sp;
    Tgt* t;
    size_t pathIdx;
  };
  std::vector<Res> res;
  std::map<std::wstring, std::pair<uint64_t, UINT32>> cloneSource;  // snapshot source -> chosen source
  std::set<std::pair<uint64_t, UINT32>> usedSources;
  bool needDatabase = false;

  for (auto& sp : snap.paths) {
    Tgt* t = nullptr;
    for (auto& x : tg)
      if (!x.used && x.tn.key == sp.key) { t = &x; break; }
    if (!t) {  // monitor moved to another port: match on EDID identity when unambiguous
      Tgt* cand = nullptr;
      int n = 0;
      for (auto& x : tg)
        if (!x.used && IEquals(x.tn.name, sp.name) && x.tn.mfg == sp.edidMfg && x.tn.prod == sp.edidProduct) { cand = &x; n++; }
      if (n == 1) t = cand;
    }
    if (!t) {
      Log(L"apply: '%ls' is not connected, skipping it", sp.name.c_str());
      continue;
    }
    std::wstring ck = sp.adapterPath + Format(L"#%u", sp.sourceId);
    size_t chosen = SIZE_MAX;
    auto cs = cloneSource.find(ck);
    for (size_t i = 0; i < all.size() && cs != cloneSource.end(); i++) {
      auto& p = all[i];
      if (LuidEq(p.targetInfo.adapterId, t->adapter) && p.targetInfo.id == t->id &&
          Luid64(p.sourceInfo.adapterId) == cs->second.first && p.sourceInfo.id == cs->second.second) { chosen = i; break; }
    }
    if (chosen == SIZE_MAX) {
      size_t fallback = SIZE_MAX;
      for (size_t i = 0; i < all.size(); i++) {
        auto& p = all[i];
        if (!LuidEq(p.targetInfo.adapterId, t->adapter) || p.targetInfo.id != t->id) continue;
        if (usedSources.count({Luid64(p.sourceInfo.adapterId), p.sourceInfo.id})) continue;
        if (p.sourceInfo.id == sp.sourceId) { chosen = i; break; }
        if (fallback == SIZE_MAX) fallback = i;
      }
      if (chosen == SIZE_MAX) chosen = fallback;
    }
    if (chosen == SIZE_MAX) {
      Log(L"apply: no free GPU output for '%ls', skipping it", sp.name.c_str());
      continue;
    }
    t->used = true;
    auto src = std::make_pair(Luid64(all[chosen].sourceInfo.adapterId), all[chosen].sourceInfo.id);
    usedSources.insert(src);
    cloneSource[ck] = src;
    if (sp.width == 0 || sp.height == 0) needDatabase = true;
    res.push_back({&sp, t, chosen});
  }
  if (res.empty()) {
    if (err) *err = L"None of the displays in this layout are connected";
    return false;
  }

  // Windows needs a primary display at (0,0)
  LONG offX = 0, offY = 0;
  bool hasOrigin = false;
  for (auto& r : res)
    if (r.sp->x == 0 && r.sp->y == 0 && r.sp->width) hasOrigin = true;
  if (!hasOrigin) {
    const PathSnap* pick = nullptr;
    for (auto& r : res)
      if (r.sp->width && (!pick || (UINT64)r.sp->width * r.sp->height > (UINT64)pick->width * pick->height)) pick = r.sp;
    if (pick) { offX = -pick->x; offY = -pick->y; }
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> np;
  std::vector<DISPLAYCONFIG_MODE_INFO> nm;
  std::map<std::pair<uint64_t, UINT32>, UINT32> srcModeIdx;
  for (auto& r : res) {
    const PathSnap& sp = *r.sp;
    DISPLAYCONFIG_PATH_INFO p = all[r.pathIdx];
    p.flags = DISPLAYCONFIG_PATH_ACTIVE |
              (sp.pathFlags & (DISPLAYCONFIG_PATH_PREFERRED_UNSCALED | DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE));
    auto sk = std::make_pair(Luid64(p.sourceInfo.adapterId), p.sourceInfo.id);
    auto it = srcModeIdx.find(sk);
    if (it != srcModeIdx.end()) {
      p.sourceInfo.modeInfoIdx = it->second;
    } else {
      DISPLAYCONFIG_MODE_INFO m{};
      m.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
      m.id = p.sourceInfo.id;
      m.adapterId = p.sourceInfo.adapterId;
      m.sourceMode.width = sp.width;
      m.sourceMode.height = sp.height;
      m.sourceMode.pixelFormat = (DISPLAYCONFIG_PIXELFORMAT)(sp.pixelFormat ? sp.pixelFormat : DISPLAYCONFIG_PIXELFORMAT_32BPP);
      m.sourceMode.position.x = sp.x + offX;
      m.sourceMode.position.y = sp.y + offY;
      nm.push_back(m);
      p.sourceInfo.modeInfoIdx = (UINT32)nm.size() - 1;
      srcModeIdx[sk] = p.sourceInfo.modeInfoIdx;
    }
    if (sp.hasTarget) {
      DISPLAYCONFIG_MODE_INFO m{};
      m.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
      m.id = p.targetInfo.id;
      m.adapterId = p.targetInfo.adapterId;
      m.targetMode.targetVideoSignalInfo = sp.signal;
      nm.push_back(m);
      p.targetInfo.modeInfoIdx = (UINT32)nm.size() - 1;
    } else {
      p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }
    if (sp.rotation) p.targetInfo.rotation = (DISPLAYCONFIG_ROTATION)sp.rotation;
    if (sp.scaling) p.targetInfo.scaling = (DISPLAYCONFIG_SCALING)sp.scaling;
    if (sp.refresh.Denominator) p.targetInfo.refreshRate = sp.refresh;
    if (sp.scanline) p.targetInfo.scanLineOrdering = (DISPLAYCONFIG_SCANLINE_ORDERING)sp.scanline;
    np.push_back(p);
  }

  const UINT32 kSupplied = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE;
  LONG r = ERROR_GEN_FAILURE;
  if (!needDatabase) {
    // 1) exact layout: positions, resolutions and signal timings as saved
    r = TrySet(L"exact", np, nm, kSupplied);
    if (r == ERROR_SUCCESS) { if (how) *how = L"exact"; return true; }
    // 2) keep desktop layout, let Windows pick the signal timing
    std::vector<DISPLAYCONFIG_PATH_INFO> np2 = np;
    std::vector<DISPLAYCONFIG_MODE_INFO> nm2;
    std::map<UINT32, UINT32> remap;
    for (UINT32 i = 0; i < nm.size(); i++)
      if (nm[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) { remap[i] = (UINT32)nm2.size(); nm2.push_back(nm[i]); }
    for (auto& p : np2) {
      p.sourceInfo.modeInfoIdx = remap[p.sourceInfo.modeInfoIdx];
      p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }
    r = TrySet(L"desktop-only", np2, nm2, kSupplied);
    if (r == ERROR_SUCCESS) { if (how) *how = L"desktop-only"; return true; }
  }
  // 3) just the set of displays; Windows restores the rest from its own database
  std::vector<DISPLAYCONFIG_PATH_INFO> np3 = np;
  for (auto& p : np3) {
    p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
  }
  r = TrySet(L"database", np3, {}, SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES);
  if (r == ERROR_SUCCESS) {
    if (how) *how = L"database";
    if (needDatabase) return true;
    // put displays back where the layout wants them (desktop moves only, no mode change)
    DisplaySnap cur;
    if (CaptureSnap(cur)) {
      bool changed = false;
      for (auto& c : cur.paths) {
        const PathSnap* w = snap.Find(c.key);
        if (w && w->width == c.width && w->height == c.height && (c.x != w->x + offX || c.y != w->y + offY)) {
          c.x = w->x + offX;
          c.y = w->y + offY;
          changed = true;
        }
      }
      static int depth = 0;
      if (changed && depth == 0) {
        std::wstring e2;
        depth++;
        if (ApplySnap(cur, &e2)) { if (how) *how = L"database+positions"; }
        depth--;
      }
    }
    return true;
  }
  if (err) {
    *err = Format(L"Windows rejected the display layout (error %ld)", r);
  }
  return false;
}

bool DisableMonitor(const std::wstring& key, const std::wstring& preferredPrimary, std::wstring* err) {
  DisplaySnap cur;
  if (!CaptureSnap(cur)) { if (err) *err = L"Could not read the display configuration"; return false; }
  size_t before = cur.paths.size();
  bool wasPrimary = false;
  for (auto& p : cur.paths)
    if (p.key == key && p.x == 0 && p.y == 0) wasPrimary = true;
  cur.paths.erase(std::remove_if(cur.paths.begin(), cur.paths.end(), [&](const PathSnap& p) { return p.key == key; }),
                  cur.paths.end());
  if (cur.paths.size() == before) return true;  // already off
  if (cur.paths.empty()) { if (err) *err = L"That is the only active display"; return false; }
  if (wasPrimary) {  // hand "primary" to the preferred display, else the biggest one left
    const PathSnap* pick = preferredPrimary.empty() ? nullptr : cur.Find(preferredPrimary);
    if (!pick)
      for (auto& p : cur.paths)
        if (!pick || (UINT64)p.width * p.height > (UINT64)pick->width * pick->height) pick = &p;
    LONG ox = pick->x, oy = pick->y;
    for (auto& p : cur.paths) { p.x -= ox; p.y -= oy; }
  }
  return ApplySnap(cur, err);
}

bool EnableMonitor(const std::wstring& key, const PathSnap* memory, std::wstring* err) {
  DisplaySnap cur;
  if (!CaptureSnap(cur)) { if (err) *err = L"Could not read the display configuration"; return false; }
  if (cur.Find(key)) return true;  // already on
  PathSnap ns;
  if (memory) {
    ns = *memory;
    RECT nr = ns.Rect();
    bool overlap = false;
    LONG maxRight = 0;
    for (auto& p : cur.paths) {
      RECT pr = p.Rect(), ir;
      if (IntersectRect(&ir, &pr, &nr)) overlap = true;
      maxRight = std::max(maxRight, pr.right);
    }
    if (overlap) {  // the other screens were rearranged since: park it to the right
      Log(L"enable: remembered spot of '%ls' overlaps now, placing it on the right", ns.name.c_str());
      ns.x = maxRight;
      ns.y = 0;
    }
  } else {
    ns.key = key;  // never seen active: let Windows pick everything (width 0 -> database apply)
  }
  cur.paths.push_back(ns);
  std::wstring how;
  if (ApplySnap(cur, err, &how)) return true;
  if (memory) {  // last resort: forget our memory and let Windows decide
    cur.paths.back() = PathSnap();
    cur.paths.back().key = key;
    return ApplySnap(cur, err, &how);
  }
  return false;
}

bool SetPrimaryMonitor(const std::wstring& key, std::wstring* err) {
  DisplaySnap cur;
  if (!CaptureSnap(cur)) { if (err) *err = L"Could not read the display configuration"; return false; }
  const PathSnap* p = cur.Find(key);
  if (!p) { if (err) *err = L"That display is not active"; return false; }
  if (p->x == 0 && p->y == 0) return true;
  LONG ox = p->x, oy = p->y;
  for (auto& q : cur.paths) { q.x -= ox; q.y -= oy; }
  return ApplySnap(cur, err);
}

}  // namespace df
