// DisplayFlip - shared helpers
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <functional>
#include <algorithm>
#include <cstdint>

#define DF_VERSION L"1.0.0"
#define DF_VERSION_A "1.0.0"

namespace df {

// ---- strings ---------------------------------------------------------------
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::wstring Lower(std::wstring s);
std::wstring Trim(const std::wstring& s);
bool IEquals(const std::wstring& a, const std::wstring& b);
bool IContains(const std::wstring& hay, const std::wstring& needle);
bool IStartsWith(const std::wstring& s, const std::wstring& prefix);
// Case-insensitive glob with * and ?. An empty pattern matches everything.
bool WildMatch(const std::wstring& pattern, const std::wstring& text);
std::wstring Format(const wchar_t* fmt, ...);
std::vector<std::wstring> SplitArgs(const std::wstring& cmdline);  // honours "quotes"
std::wstring QuoteArg(const std::wstring& s);

// ---- time ------------------------------------------------------------------
ULONGLONG NowMs();                 // monotonic ms
long long BootId();                // stable for the current boot (minutes since epoch of boot)

// ---- files / paths ---------------------------------------------------------
std::wstring ExePath();
std::wstring ExeDir();
std::wstring DataDir();            // %APPDATA%\DisplayFlip (created on demand)
bool ReadFileUtf8(const std::wstring& path, std::string& out);
bool WriteFileUtf8(const std::wstring& path, const std::string& data);  // atomic replace
bool FileExists(const std::wstring& path);
ULONGLONG FileWriteTime(const std::wstring& path);
std::wstring SafeFileName(const std::wstring& name);

// ---- logging ---------------------------------------------------------------
void LogInit();
void Log(const wchar_t* fmt, ...);
std::wstring LogPath();

// ---- process helpers -------------------------------------------------------
std::wstring ProcessImageName(DWORD pid);   // "chrome.exe" (lower case)
std::wstring ProcessImagePath(DWORD pid);
bool IsProcessRunning(const std::wstring& exeName);
bool LaunchDetached(const std::wstring& commandLine, bool hidden);
bool ShellOpen(const std::wstring& target, const std::wstring& args = L"");
// Writes to the console/pipe we were started from (we are a GUI app, so attach on demand).
void ConsolePrint(const std::wstring& s);

}  // namespace df
