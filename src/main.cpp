// DisplayFlip entry point: tray app, command-line client, installer, or Stream Deck plugin.
#include "app.h"
#include <shellapi.h>

using namespace df;

static const wchar_t* kHelp =
    L"DisplayFlip " DF_VERSION L" - one-button display, window and audio switching\n"
    L"\n"
    L"  DisplayFlip.exe                         start the tray app\n"
    L"  DisplayFlip.exe monitor \"TV\" toggle     turn a display on/off (also: on, off)\n"
    L"  DisplayFlip.exe tv [toggle|on|off]      same, for the first display in settings\n"
    L"  DisplayFlip.exe profile \"Rig\"           apply a saved profile\n"
    L"  DisplayFlip.exe toggle \"Desk\" \"Rig\"     flip between two profiles\n"
    L"  DisplayFlip.exe next                    cycle profiles\n"
    L"  DisplayFlip.exe primary \"LG\"            make a display the main one\n"
    L"  DisplayFlip.exe save \"Rig\"              save the current layout as a profile\n"
    L"  DisplayFlip.exe audio \"Headphones\"      set the default sound device (mic \"...\" for input)\n"
    L"  DisplayFlip.exe restore                 put windows back after the last switch\n"
    L"  DisplayFlip.exe status | list | diag    show state (diag also writes it to the log)\n"
    L"  DisplayFlip.exe settings | log | reload | quit\n"
    L"  DisplayFlip.exe install | uninstall     install for this user (autostart + Stream Deck plugin)\n";

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; i++)
    if (!wcscmp(argv[i], L"-port") || !wcscmp(argv[i], L"-pluginUUID")) return RunStreamDeckPlugin(inst, argc, argv);

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  LogInit();

  std::wstring cmd;
  bool background = false;
  for (int i = 1; i < argc; i++) {
    if (IEquals(argv[i], L"--background")) { background = true; continue; }
    if (i == 1 && IEquals(argv[i], ExePath())) continue;  // some launchers repeat the exe path (paths > MAX_PATH)
    cmd += (cmd.empty() ? L"" : L" ") + QuoteArg(argv[i]);
  }
  LocalFree(argv);
  auto args = SplitArgs(cmd);
  std::wstring verb = args.empty() ? L"" : Lower(args[0]);
  while (!verb.empty() && (verb[0] == L'-' || verb[0] == L'/')) verb.erase(0, 1);

  if (verb == L"help" || verb == L"h" || verb == L"?") { ConsolePrint(kHelp); return 0; }
  if (verb == L"version") { ConsolePrint(L"DisplayFlip " DF_VERSION L"\n"); return 0; }
  if (verb == L"install") return InstallCommand(false);
  if (verb == L"uninstall") return UninstallCommand();

  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DisplayFlip.Instance");
  bool first = GetLastError() != ERROR_ALREADY_EXISTS;
  HWND app = FindApp();
  if (!first && !app)  // another instance is starting up right now
    for (int i = 0; i < 60 && !app; i++) { Sleep(50); app = FindApp(); }

  if (!app && first && cmd.empty()) {
    int r = RunApp(inst, L"");  // become the tray app
    if (mutex) CloseHandle(mutex);
    return r;
  }
  if (first && mutex) { CloseHandle(mutex); mutex = nullptr; }
  if (verb == L"quit" || verb == L"exit") {
    if (app) SendToApp(app, L"quit", nullptr, 3000);
    return 0;
  }
  if (!app) {  // start the tray app in the background, then hand it the command
    LaunchDetached(QuoteArg(ExePath()) + L" --background", false);
    for (int i = 0; i < 100 && !app; i++) { Sleep(30); app = FindApp(); }
    if (!app) { ConsolePrint(L"DisplayFlip did not start\n"); return 1; }
  }
  AllowSetForegroundWindow(ASFW_ANY);  // lets the app take/hand back focus (menu, restoring your window)
  if (cmd.empty()) {
    if (!background) SendToApp(app, L"menu", nullptr, 2000);  // opened again (Start menu): show the tray menu
    return 0;
  }
  std::string reply = QueryApp(app, cmd, 15000);
  if (!reply.empty()) ConsolePrint(Widen(reply) + L"\n");
  return reply.rfind("error", 0) == 0 ? 1 : 0;
}
