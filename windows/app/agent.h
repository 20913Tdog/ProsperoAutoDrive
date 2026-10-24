// Prospero AutoDrive - Win32 layer of ProsperoAutoDrive.exe (phase 3, P3.2)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// Carries out what the state machine (core/machine.h) decides: service
// channel, heartbeat, the drive letter, the TCP test of the share port.
// One thread; between events it sleeps in WaitForMultipleObjects until the
// machine's next deadline, so it costs nothing while idle.

#pragma once

#include "client.h"
#include "machine.h"

#include <cstdio>
#include <string>

namespace prospero {

struct Settings {
  std::wstring dir;              // state folder: settings.ini, log.txt
  std::string ip;                // last address of the server
  std::string server_id;         // empty until the first connect
  std::string share_host;        // name in \\host\share, default: ip (CI: an alias)
  unsigned short port = kServicePort;

  bool Load();                   // from dir\settings.ini; false if there is none
  bool Save() const;
};

class Agent {
 public:
  Agent(Settings s, bool console);
  ~Agent();

  // Runs until `stop` is signalled; then removes the drive. Returns 0.
  int Run(HANDLE stop);

  void Log(const char *fmt, ...);

 private:
  using Action = core::Action;
  using Event = core::Event;

  void Do(const std::vector<Action> &acts);
  Event Exec(const Action &a, bool *answered);
  Event Connect();
  Event Map();
  void Unmap();
  bool Probe(unsigned short port);
  void Notify(core::Note n, const std::string &text);
  std::wstring Remote() const;

  Settings s_;
  bool console_;
  FILE *log_ = nullptr;
  Channel ch_;
  Hello hello_;
  Credential cred_;
  HANDLE sock_event_ = nullptr;
  HANDLE net_event_ = nullptr;
  HANDLE net_notify_ = nullptr;
  std::wstring letter_;          // "P:" while mapped
  core::Machine *m_ = nullptr;
};

}  // namespace prospero
