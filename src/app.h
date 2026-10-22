// The resident tray app: hotkeys, commands, display/window/audio transitions.
#pragma once
#include "common.h"

namespace df {

extern const wchar_t* kMainClass;
const ULONG_PTR kCdCommand = 0xDF01;  // client -> app: UTF-8 command line; wParam = reply window
const ULONG_PTR kCdReply = 0xDF02;    // app -> client: UTF-8 reply text
const ULONG_PTR kCdStatus = 0xDF03;   // app -> subscribers: UTF-8 status JSON

int RunApp(HINSTANCE inst, const std::wstring& firstCommand);

// client side
HWND FindApp();
bool SendToApp(HWND app, const std::wstring& command, HWND replyTo, DWORD timeoutMs);
// Sends a command and waits for its reply text (empty on failure).
std::string QueryApp(HWND app, const std::wstring& command, DWORD timeoutMs);

// installer helpers (install.cpp)
std::wstring InstallDir();
std::wstring RegisteredExe();  // exe the user installed, or empty
bool SetAutostart(bool on);
bool AutostartEnabled();
int InstallCommand(bool quiet);
int UninstallCommand();
bool InstallStreamDeckPlugin(std::wstring* msg);

// Stream Deck plugin mode (streamdeck.cpp)
int RunStreamDeckPlugin(HINSTANCE inst, int argc, wchar_t** argv);

}  // namespace df
