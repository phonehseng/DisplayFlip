// Default audio device switching (Core Audio + the IPolicyConfig interface the
// Windows Sound control panel itself uses).
#pragma once
#include "common.h"

namespace df {

struct AudioDevice {
  std::wstring id;
  std::wstring name;   // "Philips GA4 (NVIDIA High Definition Audio)"
  std::wstring desc;   // "Philips GA4"
  std::wstring iface;  // "NVIDIA High Definition Audio"
  DWORD state = 0;     // DEVICE_STATE_*
  bool active() const { return state == 1; }
};

class Audio {
 public:
  // notifyMsg is posted to notifyWnd whenever devices appear/disappear/change.
  bool Init(HWND notifyWnd, UINT notifyMsg);
  void Shutdown();

  std::vector<AudioDevice> List(bool capture, bool activeOnly = true);
  std::wstring DefaultId(bool capture);
  AudioDevice Default(bool capture);
  // roles: 0 = console+multimedia, 1 = all three (also "communications")
  bool SetDefault(const std::wstring& id, bool capture, bool includeCommunications);

  // Find by id, exact name, exact short name, or substring. Active devices win.
  bool Find(const std::wstring& spec, bool capture, AudioDevice& out, bool activeOnly = true);
  // The HDMI/DisplayPort audio endpoint that belongs to a monitor.
  bool FindForMonitor(const std::wstring& monitorName, AudioDevice& out);

 private:
  void* enumerator_ = nullptr;  // IMMDeviceEnumerator*
  void* client_ = nullptr;      // notification client
};

}  // namespace df
