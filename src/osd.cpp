#include "osd.h"
#include <shellscalingapi.h>
#include <cmath>

namespace df {

namespace {
const UINT_PTR kFadeTimer = 1;
const BYTE kMaxAlpha = 242;

COLORREF Accent(OsdKind k) {
  switch (k) {
    case OsdKind::Ok: return RGB(45, 212, 191);    // teal
    case OsdKind::Warn: return RGB(245, 158, 11);  // amber
    case OsdKind::Error: return RGB(239, 68, 68);  // red
    default: return RGB(96, 165, 250);             // blue
  }
}

HFONT MakeFont(int px, int weight, const wchar_t* face) {
  return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}
}  // namespace

void Osd::Init(HINSTANCE inst) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = Proc;
  wc.hInstance = inst;
  wc.lpszClassName = L"DisplayFlip.Osd";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                         wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, this);
  SetWindowLongPtrW(wnd_, GWLP_USERDATA, (LONG_PTR)this);
}

void Osd::Show(const std::wstring& title, const std::wstring& detail, OsdKind kind, int ms, const std::wstring& position) {
  if (!wnd_) return;
  title_ = title;
  detail_ = detail;
  kind_ = kind;
  holdMs_ = ms;
  position_ = position;
  alpha_ = kMaxAlpha;
  shownAt_ = NowMs();
  Render();
  ShowWindow(wnd_, SW_SHOWNOACTIVATE);
  SetWindowPos(wnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  SetTimer(wnd_, kFadeTimer, 16, nullptr);
}

void Osd::SetDetail(const std::wstring& detail) {
  if (!Visible()) return;
  detail_ = detail;
  alpha_ = kMaxAlpha;
  shownAt_ = NowMs();
  Render();
}

void Osd::Render() {
  // place on the primary display
  POINT origin{0, 0};
  HMONITOR hm = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(hm, &mi);
  UINT dpiX = 96, dpiY = 96;
  GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  double s = dpiX / 96.0;
  auto S = [&](double v) { return (int)std::lround(v * s); };

  HFONT ft = MakeFont(S(19), FW_SEMIBOLD, L"Segoe UI Variable Display");
  HFONT fd = MakeFont(S(13.5), FW_NORMAL, L"Segoe UI Variable Text");
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  SIZE st{0, 0}, sd{0, 0};
  HGDIOBJ of = SelectObject(dc, ft);
  GetTextExtentPoint32W(dc, title_.c_str(), (int)title_.size(), &st);
  SelectObject(dc, fd);
  if (!detail_.empty()) GetTextExtentPoint32W(dc, detail_.c_str(), (int)detail_.size(), &sd);
  SelectObject(dc, of);

  int padX = S(20), padY = S(14), bar = S(4), gap = S(2);
  int w = std::max<int>(st.cx, sd.cx) + padX * 2 + bar;
  w = std::max(w, S(220));
  w = std::min<int>(w, (mi.rcMonitor.right - mi.rcMonitor.left) * 9 / 10);
  int h = padY * 2 + st.cy + (detail_.empty() ? 0 : gap + sd.cy);
  int radius = S(10);

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ ob = SelectObject(dc, bmp);

  // background + accent bar
  COLORREF bg = RGB(24, 24, 27);
  HBRUSH bgb = CreateSolidBrush(bg);
  RECT all{0, 0, w, h};
  FillRect(dc, &all, bgb);
  DeleteObject(bgb);
  HBRUSH ab = CreateSolidBrush(Accent(kind_));
  RECT br{0, 0, bar, h};
  FillRect(dc, &br, ab);
  DeleteObject(ab);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(250, 250, 250));
  SelectObject(dc, ft);
  RECT tr{bar + padX, padY, w - padX, padY + st.cy};
  DrawTextW(dc, title_.c_str(), (int)title_.size(), &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  if (!detail_.empty()) {
    SelectObject(dc, fd);
    SetTextColor(dc, RGB(180, 180, 190));
    RECT dr{bar + padX, padY + st.cy + gap, w - padX, h - padY + S(2)};
    DrawTextW(dc, detail_.c_str(), (int)detail_.size(), &dr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  }
  GdiFlush();

  // rounded-corner alpha mask (anti-aliased), premultiplied
  uint32_t* px = (uint32_t*)bits;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      double cov = 1.0;
      double cx = x + 0.5, cy = y + 0.5;
      double ox = cx < radius ? radius - cx : (cx > w - radius ? cx - (w - radius) : 0);
      double oy = cy < radius ? radius - cy : (cy > h - radius ? cy - (h - radius) : 0);
      if (ox > 0 && oy > 0) {
        double d = std::sqrt(ox * ox + oy * oy);
        cov = std::max(0.0, std::min(1.0, radius - d + 0.5));
      }
      uint32_t c = px[y * w + x];
      uint32_t a = (uint32_t)std::lround(cov * 255);
      uint32_t r = ((c >> 16) & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, b = (c & 0xFF) * a / 255;
      px[y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }

  int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
  int x = mi.rcMonitor.left + (mw - w) / 2;
  int y;
  if (IEquals(position_, L"top")) y = mi.rcWork.top + S(48);
  else if (IEquals(position_, L"center")) y = mi.rcMonitor.top + (mh - h) / 2;
  else y = mi.rcWork.bottom - h - S(72);

  POINT dst{x, y}, src{0, 0};
  SIZE sz{w, h};
  BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)alpha_, AC_SRC_ALPHA};
  UpdateLayeredWindow(wnd_, screen, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA);

  SelectObject(dc, ob);
  DeleteObject(bmp);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  DeleteObject(ft);
  DeleteObject(fd);
}

LRESULT CALLBACK Osd::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Osd* self = (Osd*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_TIMER && self && w == kFadeTimer) {
    ULONGLONG el = NowMs() - self->shownAt_;
    if (el < (ULONGLONG)self->holdMs_) return 0;
    int fade = (int)(el - self->holdMs_);
    int a = kMaxAlpha - fade * kMaxAlpha / 220;
    if (a <= 0) {
      self->alpha_ = 0;
      KillTimer(h, kFadeTimer);
      ShowWindow(h, SW_HIDE);
      return 0;
    }
    self->alpha_ = a;
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)a, AC_SRC_ALPHA};
    UpdateLayeredWindow(h, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
    return 0;
  }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcW(h, m, w, l);
}

}  // namespace df
