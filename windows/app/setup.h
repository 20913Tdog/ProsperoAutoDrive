// Prospero AutoDrive - setup and removal of ProsperoAutoDrive.exe (P3.3)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// The exe is its own installer (docs/entwurf-phase3.md, decisions of
// 24.10.2026): a dialog with a freely chosen folder, by default
// C:\Program Files\ProsperoAutoDrive. Only a folder the user cannot write
// to needs admin rights; then just the copy and the machine-wide entries
// run elevated, the program itself never does. Removal goes through the
// Windows list of installed apps.

#pragma once

#include <windows.h>

#include <string>

namespace prospero::setup {

constexpr wchar_t kExeName[] = L"ProsperoAutoDrive.exe";

struct Installed {
  bool found = false;
  bool machine = false;          // entry under HKLM (installed with admin rights)
  std::wstring dir;
};

Installed FindInstalled();
std::wstring SelfPath();
std::wstring SelfDir();
std::wstring StateDir();         // %LOCALAPPDATA%\ProsperoAutoDrive
bool Elevated();

// The interactive installer; returns the process exit code.
int SetupDialog();

// Without a dialog (CI, scripts). launch: start the installed program.
int Install(const std::wstring &dir, bool startmenu, bool autostart, bool launch, bool quiet);
// The elevated half of Install(), started by it with admin rights.
int InstallFiles(const std::wstring &dir, bool machine, bool startmenu, bool autostart);

// From the Windows list of installed apps.
int Uninstall(bool quiet);
// The elevated half of Uninstall().
int UninstallFiles();

// Autostart for the current user (HKCU Run).
bool AutostartOn();
void SetAutostart(bool on);
// Once per user: takes the choice made at installation.
void ApplyAutostartChoice();

// Ends a running instance in this logon session and waits until its drive
// is gone. Returns false if one runs and does not end.
bool StopRunning(DWORD wait_ms);

}  // namespace prospero::setup
