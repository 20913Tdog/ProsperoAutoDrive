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
#include <functional>
#include <mutex>
#include <string>

namespace prospero {

struct Settings {
  std::wstring dir;              // state folder: settings.ini, log.txt
  std::string ip;                // last address of the server
  std::string server_id;         // empty until the first connect
  std::string share_host;        // name in \\host\share, default: ip (CI: an alias)
  std::string name;              // the server's own name (HELLO), for the tray
  unsigned short port = kServicePort;

  bool Load();                   // from dir\settings.ini; false if there is none
  bool Save() const;
};

// What the tray shows; sent on every change, from the agent's thread.
struct Status {
  bool connected = false;
  std::string letter;            // "P:"
  std::string link;              // wired, wifi, unknown
  std::string name;              // of the server, "Server" until known
  std::string last_seen;         // local time of the last connection, or empty
  std::string message;           // a notification to show now, or empty
  bool problem = false;          // message is a problem, not news
  std::string other_server;      // ID of another server at the address, or empty
};

class Agent {
 public:
  Agent(Settings s, bool console);
  ~Agent();

  // Runs until `stop` is signalled; then removes the drive. Returns 0.
  // `search` asks for a network search (tray menu).
  int Run(HANDLE stop, HANDLE search = nullptr);

  // Called on every change of the status, from the thread of Run().
  std::function<void(const Status &)> on_status;

  // Asks the server to accept one more PC for a while, over a channel of
  // its own; blocks for at most a few seconds. Returns the message to show.
  std::string OpenPairing();

  // Asks the server to end in order, over a channel of its own; blocks for
  // at most a few seconds. Returns a message to show, empty on success.
  std::string QuitServer();

  // The user confirmed Status::other_server: forget the paired server and
  // pair with that one. Any thread; the agent's thread does the work.
  void Adopt() { SetEvent(adopt_event_); }

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
  void Report(const std::string &message, bool problem);
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
  HANDLE adopt_event_ = nullptr;
  std::wstring letter_;          // "P:" while mapped
  DWORD last_map_err_ = 0;       // logged once while it repeats
  int64_t last_pong_ = 0;        // GetTickCount64 of the last PONG
  std::string last_seen_;        // local time of the last connect
  bool reported_connected_ = false;
  std::string reported_other_;
  std::mutex log_lock_;
  core::Machine *m_ = nullptr;
};

}  // namespace prospero
