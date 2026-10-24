// Prospero AutoDrive - windows of ProsperoAutoDrive.exe (P3.3)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// The tray icon with its menu and notifications, and the small dialog that
// finds the server on the first start. Plain Win32, no resources: the icon
// is drawn at run time (no binary files in git).

#pragma once

#include "agent.h"

#include <functional>
#include <string>

namespace prospero::ui {

// First start: searches the network once and lets the user confirm or type
// the address. Returns false if the user cancels.
bool AskServer(Settings *s);

// Runs the tray until "Beenden", logoff or `stop`. `start` starts the
// thread of agent->Run() once the tray listens; it is waited for before
// returning.
int RunTray(Agent *agent, HANDLE stop, HANDLE search, const std::function<HANDLE()> &start,
            const std::wstring &log_file);

}  // namespace prospero::ui
