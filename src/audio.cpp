#include "audio.h"
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propvarutil.h>

// Undocumented but stable since Windows 7; used by the Sound control panel.
// Must NOT live in an anonymous namespace: with internal linkage GCC sees that
// nothing implements it, treats calls as unreachable at -O2 and emits a jump to nowhere.
struct IPolicyConfig : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

namespace df {

namespace {

const CLSID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidNotification = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
const CLSID kClsidPolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
const IID kIidPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kDeviceDesc = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
const PROPERTYKEY kIfaceName = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

class NotifyClient : public IMMNotificationClient {
 public:
  NotifyClient(HWND w, UINT m) : wnd_(w), msg_(m) {}
  virtual ~NotifyClient() = default;
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&ref_);
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIidNotification)) {
      *ppv = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Poke(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Poke(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Poke(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { Poke(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  void Poke() { PostMessageW(wnd_, msg_, 0, 0); }
  HWND wnd_;
  UINT msg_;
  LONG ref_ = 1;
};

std::wstring Prop(IPropertyStore* ps, const PROPERTYKEY& k) {
  PROPVARIANT v;
  PropVariantInit(&v);
  std::wstring r;
  if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt == VT_LPWSTR && v.pwszVal) r = v.pwszVal;
  PropVariantClear(&v);
  return r;
}

IMMDeviceEnumerator* E(void* p) { return (IMMDeviceEnumerator*)p; }

}  // namespace

bool Audio::Init(HWND notifyWnd, UINT notifyMsg) {
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, (void**)&en))) {
    Log(L"audio: no MMDeviceEnumerator");
    return false;
  }
  enumerator_ = en;
  if (notifyWnd) {
    NotifyClient* c = new NotifyClient(notifyWnd, notifyMsg);
    if (SUCCEEDED(en->RegisterEndpointNotificationCallback(c))) client_ = c;
    else c->Release();
  }
  return true;
}

void Audio::Shutdown() {
  if (enumerator_ && client_) E(enumerator_)->UnregisterEndpointNotificationCallback((IMMNotificationClient*)client_);
  if (client_) ((NotifyClient*)client_)->Release();
  if (enumerator_) E(enumerator_)->Release();
  client_ = enumerator_ = nullptr;
}

std::vector<AudioDevice> Audio::List(bool capture, bool activeOnly) {
  std::vector<AudioDevice> out;
  if (!enumerator_) return out;
  IMMDeviceCollection* col = nullptr;
  DWORD mask = activeOnly ? DEVICE_STATE_ACTIVE : (DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED | DEVICE_STATE_DISABLED);
  if (FAILED(E(enumerator_)->EnumAudioEndpoints(capture ? eCapture : eRender, mask, &col))) return out;
  UINT n = 0;
  col->GetCount(&n);
  for (UINT i = 0; i < n; i++) {
    IMMDevice* d = nullptr;
    if (FAILED(col->Item(i, &d))) continue;
    AudioDevice a;
    LPWSTR id = nullptr;
    if (SUCCEEDED(d->GetId(&id))) { a.id = id; CoTaskMemFree(id); }
    d->GetState(&a.state);
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
      a.name = Prop(ps, kFriendlyName);
      a.desc = Prop(ps, kDeviceDesc);
      a.iface = Prop(ps, kIfaceName);
      ps->Release();
    }
    d->Release();
    out.push_back(a);
  }
  col->Release();
  return out;
}

std::wstring Audio::DefaultId(bool capture) {
  if (!enumerator_) return {};
  IMMDevice* d = nullptr;
  std::wstring r;
  if (SUCCEEDED(E(enumerator_)->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &d))) {
    LPWSTR id = nullptr;
    if (SUCCEEDED(d->GetId(&id))) { r = id; CoTaskMemFree(id); }
    d->Release();
  }
  return r;
}

AudioDevice Audio::Default(bool capture) {
  std::wstring id = DefaultId(capture);
  for (auto& d : List(capture))
    if (d.id == id) return d;
  AudioDevice none;
  none.id = id;
  return none;
}

bool Audio::SetDefault(const std::wstring& id, bool capture, bool includeCommunications) {
  (void)capture;
  IPolicyConfig* pc = nullptr;
  if (FAILED(CoCreateInstance(kClsidPolicyConfig, nullptr, CLSCTX_ALL, kIidPolicyConfig, (void**)&pc))) {
    Log(L"audio: IPolicyConfig unavailable");
    return false;
  }
  HRESULT h1 = pc->SetDefaultEndpoint(id.c_str(), eConsole);
  HRESULT h2 = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
  HRESULT h3 = includeCommunications ? pc->SetDefaultEndpoint(id.c_str(), eCommunications) : S_OK;
  pc->Release();
  return SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3);
}

bool Audio::Find(const std::wstring& specIn, bool capture, AudioDevice& out, bool activeOnly) {
  std::wstring spec = Trim(specIn);
  if (spec.empty()) return false;
  auto all = List(capture, false);
  // ranking: id > exact name > exact short name > substring; active before inactive
  for (int pass = 0; pass < 2; pass++) {
    bool wantActive = pass == 0;
    if (!wantActive && activeOnly) break;
    for (auto& d : all)
      if (d.active() == wantActive && IEquals(d.id, spec)) { out = d; return true; }
    for (auto& d : all)
      if (d.active() == wantActive && IEquals(d.name, spec)) { out = d; return true; }
    for (auto& d : all)
      if (d.active() == wantActive && IEquals(d.desc, spec)) { out = d; return true; }
    for (auto& d : all)
      if (d.active() == wantActive && IContains(d.name, spec)) { out = d; return true; }
  }
  return false;
}

bool Audio::FindForMonitor(const std::wstring& monitorName, AudioDevice& out) {
  for (auto& d : List(false, true))
    if (IEquals(d.desc, monitorName)) { out = d; return true; }
  return false;
}

}  // namespace df
