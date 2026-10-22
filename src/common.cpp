#include "common.h"
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cwctype>

namespace df {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring Lower(std::wstring s) {
  if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
  return s;
}

std::wstring Trim(const std::wstring& s) {
  size_t b = 0, e = s.size();
  while (b < e && iswspace(s[b])) b++;
  while (e > b && iswspace(s[e - 1])) e--;
  return s.substr(b, e - b);
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool IContains(const std::wstring& hay, const std::wstring& needle) {
  if (needle.empty()) return true;
  return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

bool IStartsWith(const std::wstring& s, const std::wstring& prefix) {
  return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

static bool WildRec(const wchar_t* p, const wchar_t* t) {
  // iterative glob with backtracking on the last '*'
  const wchar_t* star = nullptr;
  const wchar_t* ss = nullptr;
  while (*t) {
    if (*p == L'?' || (*p && towlower(*p) == towlower(*t))) { p++; t++; continue; }
    if (*p == L'*') { star = p++; ss = t; continue; }
    if (star) { p = star + 1; t = ++ss; continue; }
    return false;
  }
  while (*p == L'*') p++;
  return *p == 0;
}

bool WildMatch(const std::wstring& pattern, const std::wstring& text) {
  if (pattern.empty()) return true;
  return WildRec(pattern.c_str(), text.c_str());
}

std::wstring Format(const wchar_t* fmt, ...) {
  wchar_t buf[2048];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf(buf, 2047, fmt, ap);
  va_end(ap);
  buf[2047] = 0;
  return buf;
}

std::vector<std::wstring> SplitArgs(const std::wstring& s) {
  std::vector<std::wstring> out;
  std::wstring cur;
  bool inq = false, have = false;
  for (size_t i = 0; i < s.size(); i++) {
    wchar_t c = s[i];
    if (c == L'\\' && i + 1 < s.size() && s[i + 1] == L'"') { cur += L'"'; i++; have = true; continue; }
    if (c == L'"') { inq = !inq; have = true; continue; }
    if (!inq && iswspace(c)) {
      if (have) { out.push_back(cur); cur.clear(); have = false; }
      continue;
    }
    cur += c;
    have = true;
  }
  if (have) out.push_back(cur);
  return out;
}

std::wstring QuoteArg(const std::wstring& s) {
  if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
  std::wstring r = L"\"";
  for (wchar_t c : s) {
    if (c == L'"') r += L"\\\"";
    else r += c;
  }
  return r + L"\"";
}

ULONGLONG NowMs() { return GetTickCount64(); }

long long BootId() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  long long nowMs = (long long)(u.QuadPart / 10000ULL);
  long long boot = nowMs - (long long)GetTickCount64();
  return boot / 60000;  // minutes; callers allow +-2 of drift
}

std::wstring ExePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  return std::wstring(buf, n);
}

std::wstring ExeDir() {
  std::wstring p = ExePath();
  size_t k = p.find_last_of(L"\\/");
  return k == std::wstring::npos ? L"." : p.substr(0, k);
}

std::wstring DataDir() {
  static std::wstring dir;
  if (!dir.empty()) return dir;
  wchar_t* app = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app))) {
    dir = std::wstring(app) + L"\\DisplayFlip";
    CoTaskMemFree(app);
  } else {
    dir = ExeDir();
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  CreateDirectoryW((dir + L"\\profiles").c_str(), nullptr);
  return dir;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  GetFileSizeEx(h, &sz);
  out.resize((size_t)sz.QuadPart);
  DWORD rd = 0;
  BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out.resize(rd);
  if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
    out.erase(0, 3);
  return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wr = 0;
  BOOL ok = data.empty() ? TRUE : WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
  CloseHandle(h);
  if (!ok) { DeleteFileW(tmp.c_str()); return false; }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool FileExists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

ULONGLONG FileWriteTime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
  return ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

std::wstring SafeFileName(const std::wstring& name) {
  std::wstring r;
  for (wchar_t c : name) r += (wcschr(L"<>:\"/\\|?*", c) || c < 32) ? L'_' : c;
  return r;
}

// ---- logging ---------------------------------------------------------------
static CRITICAL_SECTION g_logLock;
static bool g_logInit = false;

std::wstring LogPath() { return DataDir() + L"\\displayflip.log"; }

void LogInit() {
  if (g_logInit) return;
  InitializeCriticalSection(&g_logLock);
  g_logInit = true;
  // keep the log small: roll over at ~1 MB
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (GetFileAttributesExW(LogPath().c_str(), GetFileExInfoStandard, &d) && d.nFileSizeLow > 1024 * 1024) {
    std::wstring old = DataDir() + L"\\displayflip.old.log";
    MoveFileExW(LogPath().c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
}

void Log(const wchar_t* fmt, ...) {
  if (!g_logInit) LogInit();
  wchar_t msg[4096];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf(msg, 4095, fmt, ap);
  va_end(ap);
  msg[4095] = 0;
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t line[4200];
  _snwprintf(line, 4199, L"%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %ls\r\n", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), msg);
  line[4199] = 0;
  std::string u = Narrow(line);
  EnterCriticalSection(&g_logLock);
  HANDLE h = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD wr;
    WriteFile(h, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(h);
  }
  LeaveCriticalSection(&g_logLock);
#ifdef DF_DEBUG
  OutputDebugStringW(line);
#endif
}

// ---- processes -------------------------------------------------------------
std::wstring ProcessImagePath(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return {};
  wchar_t buf[MAX_PATH * 2];
  DWORD n = MAX_PATH * 2;
  std::wstring r;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) r.assign(buf, n);
  CloseHandle(h);
  return r;
}

std::wstring ProcessImageName(DWORD pid) {
  std::wstring p = ProcessImagePath(pid);
  size_t k = p.find_last_of(L"\\/");
  return Lower(k == std::wstring::npos ? p : p.substr(k + 1));
}

bool IsProcessRunning(const std::wstring& exeName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (IEquals(pe.szExeFile, exeName)) { found = true; break; }
  }
  CloseHandle(snap);
  return found;
}

bool LaunchDetached(const std::wstring& commandLine, bool hidden) {
  STARTUPINFOW si = {sizeof(si)};
  if (hidden) {
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
  }
  PROCESS_INFORMATION pi = {};
  std::wstring cl = commandLine;
  BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE,
                           CREATE_NEW_PROCESS_GROUP | (hidden ? CREATE_NO_WINDOW : 0), nullptr, nullptr, &si, &pi);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

bool ShellOpen(const std::wstring& target, const std::wstring& args) {
  return (INT_PTR)ShellExecuteW(nullptr, L"open", target.c_str(), args.empty() ? nullptr : args.c_str(), nullptr,
                                SW_SHOWNORMAL) > 32;
}

void ConsolePrint(const std::wstring& s) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    h = GetStdHandle(STD_OUTPUT_HANDLE);
  }
  if (!h || h == INVALID_HANDLE_VALUE) return;
  DWORD w;
  if (GetFileType(h) == FILE_TYPE_CHAR) {
    WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &w, nullptr);
  } else {
    std::string u = Narrow(s);
    WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
  }
}

}  // namespace df
