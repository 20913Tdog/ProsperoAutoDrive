// Prospero AutoDrive - ProsperoAutoDrive.exe (phase 3)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// P3.2: started from the command line, without tray, setup or autostart
// (those are P3.3). It keeps the server's share as a drive letter while the
// server is reachable (docs/entwurf-phase3.md).
//
//   ProsperoAutoDrive.exe [--ip <IPv4>] [--port N] [--share-host <name>]
//                         [--state-dir <folder>] [--console]
//   ProsperoAutoDrive.exe --stop [--state-dir <folder>]
//
// --ip is needed once; it is kept in <state>\settings.ini with the server
// ID. The state folder is %LOCALAPPDATA%\ProsperoAutoDrive. --console also
// writes the log to the console. --stop ends a running instance and waits
// until its drive is gone. One instance per logon.

#include "agent.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

HANDLE g_stop = nullptr;
HANDLE g_done = nullptr;

BOOL WINAPI OnConsole(DWORD type) {
  SetEvent(g_stop);
  // On closing the window Windows ends the process once this returns;
  // give the drive time to go first.
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) WaitForSingleObject(g_done, 5000);
  return TRUE;
}

std::wstring Wide(const char *s) {
  std::wstring w;
  for (; *s; s++) w += static_cast<unsigned char>(*s);
  return w;
}

}  // namespace

int main(int argc, char **argv) {
  prospero::Settings s;
  wchar_t base[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  s.dir = (n && n < MAX_PATH ? std::wstring(base) : std::wstring(L".")) + L"\\ProsperoAutoDrive";
  std::string ip, share_host;
  int port = 0;
  bool console = false, stop = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--ip") && i + 1 < argc) {
      ip = argv[++i];
    } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--share-host") && i + 1 < argc) {
      share_host = argv[++i];
    } else if (!std::strcmp(argv[i], "--state-dir") && i + 1 < argc) {
      s.dir = Wide(argv[++i]);
    } else if (!std::strcmp(argv[i], "--console")) {
      console = true;
    } else if (!std::strcmp(argv[i], "--stop")) {
      stop = true;
    } else {
      std::printf("Aufruf: ProsperoAutoDrive.exe [--ip <IPv4-Adresse>] [--port N] [--share-host <Name>]\n"
                  "                              [--state-dir <Ordner>] [--console]\n"
                  "       ProsperoAutoDrive.exe --stop\n");
      return 2;
    }
  }

  g_stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ProsperoAutoDrive.Stop");
  if (stop) {
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ProsperoAutoDrive.Instance");
    if (!running) {
      std::printf("ProsperoAutoDrive laeuft nicht.\n");
      return 0;
    }
    SetEvent(g_stop);
    DWORD w = WaitForSingleObject(running, 15000);
    std::printf(w == WAIT_OBJECT_0 || w == WAIT_ABANDONED ? "ProsperoAutoDrive beendet.\n"
                                                          : "ProsperoAutoDrive reagiert nicht.\n");
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED ? 0 : 1;
  }
  HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\ProsperoAutoDrive.Instance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    std::printf("ProsperoAutoDrive laeuft schon.\n");
    return 1;
  }
  ResetEvent(g_stop);

  s.Load();
  if (!ip.empty()) s.ip = ip;
  if (port > 0) s.port = static_cast<unsigned short>(port);
  s.share_host = share_host;
  if (s.ip.empty()) {
    std::printf("Noch kein Server bekannt: einmal mit --ip <Adresse> starten.\n");
    return 2;
  }
  if (!s.Save()) std::printf("Einstellungen nicht gespeichert.\n");

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::printf("WSAStartup fehlgeschlagen\n");
    return 1;
  }
  g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  SetConsoleCtrlHandler(OnConsole, TRUE);
  int rc;
  {
    prospero::Agent agent(s, console);
    rc = agent.Run(g_stop);
  }
  SetEvent(g_done);
  WSACleanup();
  ReleaseMutex(instance);
  CloseHandle(instance);
  return rc;
}
