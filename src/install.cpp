// install / uninstall / autostart / Stream Deck plugin deployment
#include "app.h"
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

namespace df {

namespace {
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kAppKey = L"Software\\DisplayFlip";
const wchar_t* kPluginId = L"com.displayflip.sdPlugin";

std::wstring Known(REFKNOWNFOLDERID id) {
  wchar_t* p = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

bool DirExists(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Files can be briefly locked (antivirus scanning a file we just wrote, Stream Deck
// still shutting down), so every copy retries for a few seconds before giving up.
// \\?\ lifts the 260-character MAX_PATH limit for deep folders
std::wstring Long(const std::wstring& p) {
  if (p.size() < 240 || p.rfind(L"\\\\?\\", 0) == 0) return p;
  return L"\\\\?\\" + p;
}

bool CopyWithRetry(const std::wstring& src, const std::wstring& dst, int tries = 40) {
  DWORD err = 0;
  for (int i = 0; i < tries; i++) {
    if (CopyFileW(Long(src).c_str(), Long(dst).c_str(), FALSE)) return true;
    err = GetLastError();
    Sleep(100);
  }
  Log(L"copy %ls -> %ls failed (error %lu)", src.c_str(), dst.c_str(), err);
  return false;
}

bool CopyTree(const std::wstring& src, const std::wstring& dst) {
  SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileW(Long(src + L"\\*").c_str(), &fd);
  if (f == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  do {
    std::wstring n = fd.cFileName;
    if (n == L"." || n == L"..") continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = CopyTree(src + L"\\" + n, dst + L"\\" + n) && ok;
    else ok = CopyWithRetry(src + L"\\" + n, dst + L"\\" + n, 20) && ok;
  } while (FindNextFileW(f, &fd));
  FindClose(f);
  return ok;
}

void StartStreamDeck(const std::wstring& exe) {
  // CreateProcess, not ShellExecute: the installer exits right away, which can abort a ShellExecute launch
  if (FileExists(exe) && !LaunchDetached(QuoteArg(exe) + L" --runinbk", false)) Log(L"could not restart Stream Deck");
}

// Terminates processes whose image path starts with `prefix` (or equals it). Returns how many.
int KillByPath(const std::wstring& prefix, std::wstring* lastPath = nullptr) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  int n = 0;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (pe.th32ProcessID == GetCurrentProcessId()) continue;
    std::wstring path = ProcessImagePath(pe.th32ProcessID);
    if (path.empty() || !IStartsWith(path, prefix)) continue;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
    if (!h) continue;
    if (lastPath) *lastPath = path;
    TerminateProcess(h, 0);
    WaitForSingleObject(h, 3000);
    CloseHandle(h);
    n++;
  }
  CloseHandle(snap);
  return n;
}

std::wstring RunningImage(const std::wstring& exeName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return {};
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  std::wstring r;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (IEquals(pe.szExeFile, exeName)) { r = ProcessImagePath(pe.th32ProcessID); break; }
  CloseHandle(snap);
  return r;
}

bool StopRunningApp() {
  HWND app = FindApp();
  if (!app) return true;
  SendToApp(app, L"quit", nullptr, 3000);
  for (int i = 0; i < 60 && FindApp(); i++) Sleep(50);
  return FindApp() == nullptr;
}

std::wstring PluginSourceDir() {
  std::wstring cands[] = {ExeDir() + L"\\streamdeck\\" + kPluginId, InstallDir() + L"\\streamdeck\\" + kPluginId,
                          ExeDir() + L"\\..\\streamdeck\\" + kPluginId};
  for (auto& c : cands)
    if (FileExists(c + L"\\manifest.json")) return c;
  return {};
}
}  // namespace

std::wstring InstallDir() { return Known(FOLDERID_LocalAppData) + L"\\Programs\\DisplayFlip"; }

std::wstring RegisteredExe() {
  wchar_t buf[MAX_PATH * 2] = {};
  DWORD sz = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
  return buf;
}

bool SetAutostart(bool on) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
  LSTATUS r;
  if (on) {
    std::wstring exe = RegisteredExe();
    if (exe.empty() || !FileExists(exe)) exe = ExePath();
    std::wstring v = QuoteArg(exe) + L" --background";
    if (v[0] != L'"') v = L"\"" + exe + L"\" --background";
    r = RegSetValueExW(k, L"DisplayFlip", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
  } else {
    r = RegDeleteValueW(k, L"DisplayFlip");
  }
  RegCloseKey(k);
  return r == ERROR_SUCCESS || (!on && r == ERROR_FILE_NOT_FOUND);
}

bool AutostartEnabled() {
  DWORD sz = 0;
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"DisplayFlip", RRF_RT_REG_SZ, nullptr, nullptr, &sz) == ERROR_SUCCESS;
}

bool InstallStreamDeckPlugin(std::wstring* msg) {
  std::wstring src = PluginSourceDir();
  if (src.empty()) {
    if (msg) *msg = L"Plugin files not found next to DisplayFlip.exe";
    return false;
  }
  std::wstring dst = Known(FOLDERID_RoamingAppData) + L"\\Elgato\\StreamDeck\\Plugins\\" + kPluginId;
  std::wstring sdExe = RunningImage(L"StreamDeck.exe");
  bool wasRunning = !sdExe.empty();
  if (sdExe.empty()) sdExe = L"C:\\Program Files\\Elgato\\StreamDeck\\StreamDeck.exe";
  // Stream Deck reads manifests only at start. If the manifest and settings page are unchanged
  // (a normal update), swapping the exe is enough: Stream Deck relaunches a plugin that exits.
  auto same = [&](const wchar_t* rel) {
    std::string a, b;
    return ReadFileUtf8(src + rel, a) && ReadFileUtf8(dst + rel, b) && a == b;
  };
  bool light = wasRunning && same(L"\\manifest.json") && same(L"\\pi\\index.html");
  if (wasRunning && !light) KillByPath(sdExe);
  KillByPath(dst);  // the running plugin instance holds its exe open
  bool ok = CopyTree(src, dst);
  std::wstring exe = RegisteredExe();
  if (exe.empty() || !FileExists(exe)) exe = ExePath();
  ok = CopyWithRetry(exe, dst + L"\\DisplayFlip.exe") && ok;
  if (!light) StartStreamDeck(sdExe);
  Log(L"stream deck plugin install to %ls: %ls", dst.c_str(), ok ? L"ok" : L"FAILED");
  if (msg) *msg = ok ? L"Look for \"DisplayFlip\" in the Stream Deck action list" : L"Copying the plugin files failed";
  return ok;
}

int InstallCommand(bool quiet) {
  std::wstring dir = InstallDir();
  std::wstring target = dir + L"\\DisplayFlip.exe";
  SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  if (!StopRunningApp()) Log(L"install: running instance did not exit in time");
  bool self = IEquals(ExePath(), target);
  if (!self && !CopyWithRetry(ExePath(), target)) {
    ConsolePrint(L"Could not copy DisplayFlip.exe to " + target + L"\n");
    return 1;
  }
  std::wstring src = PluginSourceDir();
  if (!src.empty() && !IStartsWith(src, dir)) CopyTree(src, dir + L"\\streamdeck\\" + kPluginId);
  std::wstring readme = ExeDir() + L"\\README.md";
  if (FileExists(readme) && !self) CopyFileW(readme.c_str(), (dir + L"\\README.md").c_str(), FALSE);

  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    RegSetValueExW(k, L"InstallPath", 0, REG_SZ, (const BYTE*)target.c_str(), (DWORD)((target.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
  }
  SetAutostart(true);
  std::wstring sdMsg = L"Stream Deck not found (skipped plugin)";
  if (FileExists(L"C:\\Program Files\\Elgato\\StreamDeck\\StreamDeck.exe") || !RunningImage(L"StreamDeck.exe").empty())
    InstallStreamDeckPlugin(&sdMsg);
  LaunchDetached(QuoteArg(target) + L" --background", false);
  if (!quiet)
    ConsolePrint(L"DisplayFlip installed to " + dir + L"\n  - starts with Windows\n  - Stream Deck: " + sdMsg +
          L"\n  - settings: " + DataDir() + L"\\config.json\n");
  Log(L"installed to %ls", dir.c_str());
  return 0;
}

int UninstallCommand() {
  StopRunningApp();
  SetAutostart(false);
  std::wstring dst = Known(FOLDERID_RoamingAppData) + L"\\Elgato\\StreamDeck\\Plugins\\" + kPluginId;
  if (DirExists(dst)) {
    std::wstring sdExe = RunningImage(L"StreamDeck.exe");
    if (!sdExe.empty()) KillByPath(sdExe);
    KillByPath(dst);
    SHFILEOPSTRUCTW op = {};
    std::wstring from = dst + L'\0';
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    if (!sdExe.empty()) StartStreamDeck(sdExe);
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
  ConsolePrint(L"DisplayFlip removed from startup and Stream Deck.\nYour settings are kept in " + DataDir() +
        L"\nDelete " + InstallDir() + L" to remove the program files.\n");
  return 0;
}

}  // namespace df
