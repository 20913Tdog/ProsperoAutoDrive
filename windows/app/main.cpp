// Prospero AutoDrive - ProsperoAutoDrive.exe (phase 3)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// Keeps the server's share as a drive letter while the server is reachable,
// with a tray icon (docs/entwurf-phase3.md). The same exe is its own
// installer:
//
//   ProsperoAutoDrive.exe                 not installed here: setup dialog;
//                                         installed here: runs (autostart)
//   ProsperoAutoDrive.exe --run [--ip <IPv4>] [--port N] [--share-host <name>]
//                         [--state-dir <folder>] [--console]
//                                         runs from anywhere (tests)
//   ProsperoAutoDrive.exe --stop          ends a running instance, waits until
//                                         its drive is gone
//   ProsperoAutoDrive.exe --install <folder> [--no-autostart] [--no-startmenu]
//                         [--no-launch] [--quiet]
//                                         setup without the dialog
//   ProsperoAutoDrive.exe --uninstall [--quiet]
//                                         what "Installed apps" calls
//
// --install-files and --uninstall-files are the elevated halves of setup
// and removal. --ip is needed once if the server is not found; it is kept
// in the state folder (%LOCALAPPDATA%\ProsperoAutoDrive) with the server ID.
// One instance per logon.

#include "agent.h"
#include "setup.h"
#include "ui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

HANDLE g_stop = nullptr;

std::wstring Wide(const char *s) {
  std::wstring w;
  int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  if (n > 1) {
    w.resize(static_cast<size_t>(n - 1));
    MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
  }
  return w;
}

// A GUI program has no console; for --console, --stop and the setup
// messages it writes to the one it was started from, if any.
void AttachOutput() {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
  }
}

DWORD WINAPI AgentThread(LPVOID arg) {
  auto *a = static_cast<std::pair<prospero::Agent *, HANDLE> *>(arg);
  return static_cast<DWORD>(a->first->Run(g_stop, a->second));
}

}  // namespace

int main(int argc, char **argv) {
  namespace setup = prospero::setup;
  prospero::Settings s;
  s.dir = setup::StateDir();
  std::string ip, share_host;
  std::wstring install_dir;
  int port = 0;
  bool run = false, console = false, stop = false, quiet = false;
  bool install = false, install_files = false, machine = false, uninstall = false, uninstall_files = false;
  bool autostart = true, startmenu = true, launch = true;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    bool more = i + 1 < argc;
    if (!std::strcmp(a, "--run")) {
      run = true;
    } else if (!std::strcmp(a, "--ip") && more) {
      ip = argv[++i];
      run = true;
    } else if (!std::strcmp(a, "--port") && more) {
      port = std::atoi(argv[++i]);
      run = true;
    } else if (!std::strcmp(a, "--share-host") && more) {
      share_host = argv[++i];
      run = true;
    } else if (!std::strcmp(a, "--state-dir") && more) {
      s.dir = Wide(argv[++i]);
      run = true;
    } else if (!std::strcmp(a, "--console")) {
      console = true;
      run = true;
    } else if (!std::strcmp(a, "--stop")) {
      stop = true;
    } else if (!std::strcmp(a, "--install") && more) {
      install = true;
      install_dir = Wide(argv[++i]);
    } else if (!std::strcmp(a, "--install-files") && more) {
      install_files = true;
      install_dir = Wide(argv[++i]);
    } else if (!std::strcmp(a, "--machine")) {
      machine = true;
    } else if (!std::strcmp(a, "--no-autostart")) {
      autostart = false;
    } else if (!std::strcmp(a, "--no-startmenu")) {
      startmenu = false;
    } else if (!std::strcmp(a, "--no-launch")) {
      launch = false;
    } else if (!std::strcmp(a, "--quiet")) {
      quiet = true;
    } else if (!std::strcmp(a, "--uninstall")) {
      uninstall = true;
    } else if (!std::strcmp(a, "--uninstall-files")) {
      uninstall_files = true;
    } else {
      AttachOutput();
      std::printf("Aufruf: ProsperoAutoDrive.exe [--run] [--ip <IPv4-Adresse>] [--port N] [--share-host <Name>]\n"
                  "                              [--state-dir <Ordner>] [--console]\n"
                  "       ProsperoAutoDrive.exe --stop\n"
                  "       ProsperoAutoDrive.exe --install <Ordner> [--no-autostart] [--no-startmenu]\n"
                  "                             [--no-launch] [--quiet]\n"
                  "       ProsperoAutoDrive.exe --uninstall [--quiet]\n");
      return 2;
    }
  }

  if (stop) {
    AttachOutput();
    bool ok = setup::StopRunning(15000);
    std::printf(ok ? "ProsperoAutoDrive beendet.\n" : "ProsperoAutoDrive reagiert nicht.\n");
    return ok ? 0 : 1;
  }
  if (install_files) return setup::InstallFiles(install_dir, machine, startmenu, autostart);
  if (uninstall_files) return setup::UninstallFiles();
  if (uninstall) return setup::Uninstall(quiet);
  if (install) {
    AttachOutput();
    int rc = setup::Install(install_dir, startmenu, autostart, launch, quiet);
    std::printf(rc == 0 ? "Installiert.\n" : "Installation fehlgeschlagen, Fehler %d.\n", rc);
    return rc;
  }
  setup::Installed here = setup::FindInstalled();
  bool installed_here = here.found && _wcsicmp(here.dir.c_str(), setup::SelfDir().c_str()) == 0;
  if (!run && !installed_here) return setup::SetupDialog();
  if (console) AttachOutput();

  g_stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ProsperoAutoDrive.Stop");
  HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\ProsperoAutoDrive.Instance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (console) std::printf("ProsperoAutoDrive laeuft schon.\n");
    return 1;
  }
  ResetEvent(g_stop);
  if (installed_here) setup::ApplyAutostartChoice();

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
  s.Load();
  if (!ip.empty()) s.ip = ip;
  if (port > 0) s.port = static_cast<unsigned short>(port);
  s.share_host = share_host;
  if (s.ip.empty() && !prospero::ui::AskServer(&s)) {
    WSACleanup();
    return 2;
  }
  s.Save();

  int rc;
  {
    prospero::Agent agent(s, console);
    HANDLE search = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::pair<prospero::Agent *, HANDLE> arg{&agent, search};
    HANDLE thread = nullptr;
    rc = prospero::ui::RunTray(
        &agent, g_stop, search,
        [&] {
          thread = CreateThread(nullptr, 0, AgentThread, &arg, 0, nullptr);
          return thread;
        },
        s.dir + L"\\log.txt");
    if (thread) CloseHandle(thread);
    CloseHandle(search);
  }
  WSACleanup();
  ReleaseMutex(instance);
  CloseHandle(instance);
  return rc;
}
