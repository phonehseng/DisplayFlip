// Small click-through notification ("TV on - audio: Philips GA4"). Never takes focus.
#pragma once
#include "common.h"

namespace df {

enum class OsdKind { Info, Ok, Warn, Error };

class Osd {
 public:
  void Init(HINSTANCE inst);
  void Show(const std::wstring& title, const std::wstring& detail, OsdKind kind, int ms, const std::wstring& position);
  void SetDetail(const std::wstring& detail);  // update the text if still visible
  bool Visible() const { return wnd_ && alpha_ > 0; }

 private:
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
  void Render();
  HWND wnd_ = nullptr;
  std::wstring title_, detail_, position_;
  OsdKind kind_ = OsdKind::Info;
  int alpha_ = 0;
  int holdMs_ = 1500;
  ULONGLONG shownAt_ = 0;
};

}  // namespace df
