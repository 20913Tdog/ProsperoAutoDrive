// Prospero AutoDrive - Win32 layer of ProsperoAutoDrive.exe (phase 3, P3.2)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT

#include "agent.h"

#include "setup.h"

#include <iphlpapi.h>
#include <lm.h>
#include <winnetwk.h>

#include <cstdarg>
#include <cstdlib>

namespace prospero {

namespace {

constexpr long kLogMax = 1024 * 1024;       // then log.txt becomes log.1.txt

int64_t Now() { return static_cast<int64_t>(GetTickCount64()); }

std::wstring Wide(const std::string &s) { return std::wstring(s.begin(), s.end()); }

std::string Narrow(const std::wstring &w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
  return s;
}

std::string Ini(const std::wstring &file, const wchar_t *key) {
  wchar_t buf[256];
  GetPrivateProfileStringW(L"server", key, L"", buf, 256, file.c_str());
  return Narrow(buf);
}

// "error 53 (The network path was not found.)": the number with Windows'
// own text, in English where Windows has it, else in its own language.
std::string WinError(DWORD rc) {
  char buf[256] = {};
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  HMODULE netmsg = nullptr;
  DWORD n = FormatMessageA(flags, nullptr, rc, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), buf, sizeof buf, nullptr);
  if (!n) n = FormatMessageA(flags, nullptr, rc, 0, buf, sizeof buf, nullptr);
  if (!n && rc >= 2100 && rc < 2999) {
    // NERR_* codes of NetUseDel live in netmsg.dll.
    netmsg = LoadLibraryExW(L"netmsg.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (netmsg) n = FormatMessageA(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, netmsg, rc, 0, buf,
                                   sizeof buf, nullptr);
  }
  if (netmsg) FreeLibrary(netmsg);
  while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
  return "error " + std::to_string(rc) + (n ? " (" + std::string(buf) + ")" : "");
}

// "GOODBYE 1 reason=remote" in words.
std::string Goodbye(const std::string &line) {
  size_t p = line.find("reason=");
  std::string why = p == std::string::npos ? "" : line.substr(p + 7, line.find(' ', p) - p - 7);
  if (why == "remote") return "it was shut down from a paired PC";
  if (why == "replaced") return "a new instance of the server replaced it";
  return why.empty() ? "no reason given" : "reason: " + why;
}

void CALLBACK OnNetChange(PVOID ctx, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  SetEvent(static_cast<HANDLE>(ctx));
}

}  // namespace

bool Settings::Load() {
  std::wstring file = dir + L"\\settings.ini";
  if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  ip = Ini(file, L"ip");
  server_id = Ini(file, L"server_id");
  name = Ini(file, L"name");
  std::string p = Ini(file, L"port");
  if (!p.empty()) port = static_cast<unsigned short>(std::atoi(p.c_str()));
  return true;
}

bool Settings::Save() const {
  std::wstring file = dir + L"\\settings.ini";
  CreateDirectoryW(dir.c_str(), nullptr);
  return WritePrivateProfileStringW(L"server", L"ip", Wide(ip).c_str(), file.c_str()) &&
         WritePrivateProfileStringW(L"server", L"server_id", Wide(server_id).c_str(), file.c_str()) &&
         WritePrivateProfileStringW(L"server", L"port", std::to_wstring(port).c_str(), file.c_str()) &&
         WritePrivateProfileStringW(L"server", L"name", Wide(name).c_str(), file.c_str());
}

Agent::Agent(Settings s, bool console) : s_(std::move(s)), console_(console) {
  CreateDirectoryW(s_.dir.c_str(), nullptr);
  std::wstring file = s_.dir + L"\\log.txt";
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > kLogMax) {
    MoveFileExW(file.c_str(), (s_.dir + L"\\log.1.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  log_ = _wfopen(file.c_str(), L"ab");
  sock_event_ = WSACreateEvent();
  net_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  adopt_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  retry_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  NotifyIpInterfaceChange(AF_UNSPEC, OnNetChange, net_event_, FALSE, &net_notify_);
}

Agent::~Agent() {
  if (net_notify_) CancelMibChangeNotify2(net_notify_);
  ch_.Close();
  if (sock_event_) WSACloseEvent(sock_event_);
  if (net_event_) CloseHandle(net_event_);
  if (adopt_event_) CloseHandle(adopt_event_);
  if (retry_event_) CloseHandle(retry_event_);
  if (log_) fclose(log_);
}

void Agent::Log(const char *fmt, ...) {
  char msg[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  SYSTEMTIME t;
  GetLocalTime(&t);
  char line[700];
  snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n", t.wYear, t.wMonth, t.wDay,
           t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
  std::lock_guard<std::mutex> hold(log_lock_);
  if (log_) {
    fputs(line, log_);
    fflush(log_);
  }
  if (console_) fputs(line, stdout);
}

std::wstring Agent::Remote() const {
  return L"\\\\" + Wide(s_.share_host.empty() ? s_.ip : s_.share_host) + L"\\" + Wide(hello_.share);
}

int Agent::Run(HANDLE stop, HANDLE search) {
  core::Timing t;
  bool creds = !s_.server_id.empty() && LoadCredential(s_.server_id, &cred_);
  core::Machine m(t, s_.server_id, creds);
  m_ = &m;
  // With admin rights the drive would be invisible to the Explorer.
  Log("Started: server %s at %s, control channel port %u, %s",
      s_.server_id.empty() ? "not paired yet" : s_.server_id.c_str(), s_.ip.empty() ? "?" : s_.ip.c_str(),
      s_.port, setup::Elevated() ? "WITH administrator rights" : "without administrator rights");
  Report("", false);
  Do(m.Start(Now()));
  HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  for (;;) {
    HANDLE hs[6] = {stop, sock_event_, net_event_, search ? search : never, adopt_event_, retry_event_};
    int64_t wake = m.NextWakeMs();
    DWORD to = INFINITE;
    if (wake >= 0) {
      int64_t d = wake - Now();
      to = d > 0 ? static_cast<DWORD>(d) : 0;
    }
    DWORD r = WaitForMultipleObjects(6, hs, FALSE, to);
    if (r == WAIT_OBJECT_0) {
      Log("Exit requested");
      Do(m.Handle(Event(Event::kShutdown), Now()));
      break;
    }
    if (r == WAIT_OBJECT_0 + 1) {
      WSAResetEvent(sock_event_);
      if (!ch_.open()) continue;
      bool closed = false;
      std::string goodbye;
      int pongs = ch_.Poll(&closed, &goodbye);
      if (pongs) last_pong_ = Now();
      if (!goodbye.empty()) {
        Log("Server said goodbye: %s (%s)", Goodbye(goodbye).c_str(), goodbye.c_str());
      }
      for (int i = 0; i < pongs; i++) Do(m.Handle(Event(Event::kPong), Now()));
      if (closed) {
        Log("Control channel: closed by the server");
        ch_.Close();
        Do(m.Handle(Event(Event::kChannelClosed), Now()));
      }
    } else if (r == WAIT_OBJECT_0 + 2) {
      ResetEvent(net_event_);
      Do(m.Handle(Event(Event::kNetworkChanged), Now()));
    } else if (r == WAIT_OBJECT_0 + 3) {
      ResetEvent(search);
      Do(m.Handle(Event(Event::kSearch), Now()));
    } else if (r == WAIT_OBJECT_0 + 4) {
      ResetEvent(adopt_event_);
      std::string other = m.other_server();
      if (!other.empty()) {
        // The old pairing goes for good: that server is gone for this PC.
        Log("Pairing with the new server %s instead of %s (confirmed by the user)", other.c_str(),
            s_.server_id.c_str());
        if (!s_.server_id.empty()) DeleteCredential(s_.server_id);
        cred_ = Credential();
        s_.server_id = other;
        s_.name.clear();
        s_.Save();
        Do(m.Handle(Event(Event::kAdopt), Now()));
        Report("", false);
      }
    } else if (r == WAIT_OBJECT_0 + 5) {
      ResetEvent(retry_event_);
      Log("Pair again: requested by the user");
      Do(m.Handle(Event(Event::kRetry), Now()));
      Report("", false);
    } else if (r == WAIT_TIMEOUT) {
      Do(m.Handle(Event(Event::kTick), Now()));
    } else {
      Log("Waiting for events failed, %s", WinError(GetLastError()).c_str());
      Sleep(1000);
    }
  }
  CloseHandle(never);
  m_ = nullptr;
  Log("Stopped");
  return 0;
}

// Runs the actions; an answer that is there at once goes straight back to
// the machine, and its actions are run after the rest.
void Agent::Do(const std::vector<Action> &acts) {
  std::vector<Action> todo = acts;
  for (size_t i = 0; i < todo.size(); i++) {
    bool answered = false;
    Event e = Exec(todo[i], &answered);
    if (answered) {
      std::vector<Action> more = m_->Handle(e, Now());
      todo.insert(todo.end(), more.begin(), more.end());
    }
  }
}

Agent::Event Agent::Exec(const Action &a, bool *answered) {
  std::string err;
  *answered = true;
  switch (a.kind) {
    case Action::kConnectChannel:
      return Connect();
    case Action::kCloseChannel:
      ch_.Close();
      WSAResetEvent(sock_event_);
      break;
    case Action::kPair: {
      Event e(Event::kPaired);
      std::string refused;
      Credential c;
      if (ch_.Pair(PcName(), &c.user, &c.password, &refused, &err)) {
        c.last_ip = s_.ip;
        if (StoreCredential(hello_.server_id, c, &err)) {
          cred_ = c;
          e.ok = true;
          Log("Paired with server %s; user name and password stored in the Windows Credential Manager",
              hello_.server_id.c_str());
        } else {
          Log("Paired, but the credentials could not be stored: %s", err.c_str());
        }
      } else {
        // Tried every 10 s while refused: only a change goes into the log.
        static std::string last;
        std::string why = refused.empty() ? err : refused;
        if (why != last) {
          Log("Pairing refused by the server (%s)%s", why.c_str(),
              why == "closed" ? ": pairing is closed; restarting the server opens it for 2 minutes" : "");
        }
        last = why;
      }
      return e;
    }
    case Action::kMap:
      return Map();
    case Action::kUnmap:
      Unmap();
      break;
    case Action::kSendPing:
      if (!ch_.SendPing(cred_.password, sock_event_, &err)) {
        Log("Heartbeat could not be sent: %s", err.c_str());
        ch_.Close();
        return Event(Event::kChannelClosed);
      }
      break;
    case Action::kProbe445: {
      Event e(Event::kPort445);
      unsigned short port = hello_.smb_port ? hello_.smb_port : 445;
      e.ok = Probe(port);
      // When the last PONG came tells how long the server has been gone.
      if (last_pong_) {
        Log("SMB port %u check: %s; last heartbeat reply %lld ms ago", port,
            e.ok ? "reachable" : "no answer", static_cast<long long>(Now() - last_pong_));
      } else {
        Log("SMB port %u check: %s; no heartbeat reply so far", port, e.ok ? "reachable" : "no answer");
      }
      return e;
    }
    case Action::kBroadcast: {
      // Up to 30 s, one request every 2 s, until the server answers.
      Log("Network search: broadcasting for up to 30 s");
      std::vector<in_addr> targets;
      for (auto &t : BroadcastTargets()) targets.push_back(t.first);
      for (int round = 0; round < 15; round++) {
        std::vector<Here> found;
        if (!Discover(targets, s_.port, 2000, &found, &err)) break;
        for (const Here &h : found) {
          if (s_.server_id.empty() || h.id == s_.server_id) {
            Log("Network search: found server %s at %s", h.id.c_str(), h.ip.c_str());
            s_.ip = h.ip;
            s_.Save();
            return Event(Event::kFound);
          }
        }
      }
      Log("Network search: nothing found");
      Report((s_.name.empty() ? std::string("Server") : s_.name) + " not found on the network", true);
      break;
    }
    case Action::kNotify:
      Notify(a.note, a.text);
      break;
  }
  *answered = false;
  return Event();
}

Agent::Event Agent::Connect() {
  // Every 2 s while the console is off or the drive fails: only a change
  // goes into the log.
  static std::string last_err, last_up;
  std::string err;
  if (!ch_.Connect(s_.ip, s_.port, 1000, &err) || !ch_.SayHello(&hello_, &err)) {
    ch_.Close();
    if (err != last_err) Log("Control channel to %s:%u: %s", s_.ip.c_str(), s_.port, err.c_str());
    last_err = err;
    last_up.clear();
    return Event(Event::kChannelFailed);
  }
  last_err.clear();
  Event e(Event::kChannelUp);
  e.server_id = hello_.server_id;
  e.link = hello_.link;
  if (cred_.password.empty() || hello_.server_id != s_.server_id) {
    Credential c;
    if (LoadCredential(hello_.server_id, &c)) cred_ = c;
  }
  e.ok = !cred_.password.empty();
  std::string up = "connected to server " + hello_.server_id + " (" + hello_.name + "), its network link: " +
                   (hello_.link.empty() ? "unknown" : hello_.link);
  if (up != last_up) Log("Control channel: %s", up.c_str());
  last_up = up;
  return e;
}

Agent::Event Agent::Map() {
  std::wstring remote = Remote();
  std::wstring user = Wide(cred_.user);
  std::wstring password = Wide(cred_.password);
  // P:, else backwards from O:; a letter that is in use stays untouched,
  // unless it is our own mapping left from an earlier run.
  std::wstring chosen;
  DWORD drives = GetLogicalDrives();
  for (wchar_t l : std::wstring(L"PONMLKJIHGFED")) {
    std::wstring name = std::wstring(1, l) + L":";
    if (!(drives & (1u << (l - L'A')))) {
      chosen = name;
      break;
    }
    wchar_t cur[300];
    DWORD n = 300;
    if (WNetGetConnectionW(name.c_str(), cur, &n) == NO_ERROR && _wcsicmp(cur, remote.c_str()) == 0) {
      WNetCancelConnection2W(name.c_str(), 0, TRUE);
      chosen = name;
      break;
    }
  }
  if (chosen.empty()) {
    Event e(Event::kMapFailed);
    e.reason = Event::kNoLetter;
    return e;
  }
  NETRESOURCEW nr{};
  nr.dwType = RESOURCETYPE_DISK;
  nr.lpLocalName = chosen.data();
  nr.lpRemoteName = remote.data();
  int64_t t0 = Now();
  DWORD rc = WNetAddConnection2W(&nr, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
  if (rc != NO_ERROR) {
    Event e(Event::kMapFailed);
    e.reason = (rc == ERROR_LOGON_FAILURE || rc == ERROR_ACCESS_DENIED || rc == ERROR_INVALID_PASSWORD)
                   ? Event::kAuth
                   : Event::kOther;
    e.text = "Windows " + WinError(rc);
    if (rc != last_map_err_) {
      Log("Drive %s -> %s not connected: Windows %s", Narrow(chosen).c_str(), Narrow(remote).c_str(),
          WinError(rc).c_str());
    }
    last_map_err_ = rc;
    return e;
  }
  last_map_err_ = 0;
  letter_ = chosen;
  Log("Drive %s -> %s connected in %lld ms", Narrow(chosen).c_str(), Narrow(remote).c_str(),
      static_cast<long long>(Now() - t0));
  if (s_.server_id != hello_.server_id || s_.name != hello_.name) {
    s_.server_id = hello_.server_id;
    s_.name = hello_.name;
    s_.Save();
  }
  SYSTEMTIME lt;
  GetLocalTime(&lt);
  char seen[40];
  snprintf(seen, sizeof seen, "%04u-%02u-%02u %02u:%02u", lt.wYear, lt.wMonth, lt.wDay, lt.wHour,
           lt.wMinute);
  last_seen_ = seen;
  Event e(Event::kMapped);
  e.text = Narrow(chosen);
  if (chosen != L"P:") e.reason = Event::kLetterBusy;
  return e;
}

// NetUseDel with USE_LOTS_OF_FORCE: WNetCancelConnection2 with force took
// 5 s against a server that had gone (E2, 24.10.2026).
void Agent::Unmap() {
  if (letter_.empty()) return;
  int64_t t0 = Now();
  const char *how = "NetUseDel";
  DWORD rc = NetUseDel(nullptr, letter_.data(), USE_LOTS_OF_FORCE);
  if (rc != NERR_Success) {
    how = "WNetCancelConnection2";
    rc = WNetCancelConnection2W(letter_.c_str(), 0, TRUE);
  }
  Log("Drive %s disconnected in %lld ms (%s)%s", Narrow(letter_).c_str(), static_cast<long long>(Now() - t0),
      how, rc == NO_ERROR ? "" : (", Windows " + WinError(rc)).c_str());
  letter_.clear();
}

bool Agent::Probe(unsigned short port) {
  Channel c;
  std::string err;
  return c.Connect(s_.ip, port, 1000, &err);
}

void Agent::Notify(core::Note n, const std::string &text) {
  std::string who = s_.name.empty() ? "Server" : s_.name;
  std::string msg;
  bool problem = true;
  switch (n) {
    case core::Note::kConnected:
      msg = who + " connected as " + text;
      problem = false;
      break;
    case core::Note::kDisconnected:
      msg = who + " disconnected";
      problem = false;
      break;
    case core::Note::kOtherServer:
      msg = "A different server answers at " + s_.ip + " (server ID " + text +
            "). If it was set up again: tray menu \"Pair with the new server\".";
      break;
    case core::Note::kPairRefused:
      msg = who + " found, but pairing is not possible (probably an old pairing). "
            "Restart the server, then choose \"Pair again\" in the tray menu.";
      break;
    case core::Note::kLoginRefused:
      msg = who + ": the share refused the sign-in";
      break;
    case core::Note::kLetterBusy:
      msg = "P: is in use, the drive is " + text + " instead";
      problem = false;
      break;
    case core::Note::kNoLetter:
      msg = "No free drive letter";
      break;
    case core::Note::kMapFailed:
      msg = "Drive not connected: " + text;
      break;
  }
  Log("Notification: %s", msg.c_str());
  Report(msg, problem);
}

void Agent::Report(const std::string &message, bool problem) {
  Status st;
  st.connected = m_ && m_->mapped();
  st.letter = Narrow(letter_);
  st.link = hello_.link;
  st.name = s_.name.empty() ? "Server" : s_.name;
  st.last_seen = last_seen_;
  st.message = message;
  st.problem = problem;
  if (m_) {
    st.other_server = m_->other_server();
    st.pair_refused = m_->pair_refused();
  }
  if (on_status && (!message.empty() || st.connected != reported_connected_ || !m_ ||
                    st.other_server != reported_other_ || st.pair_refused != reported_refused_)) {
    on_status(st);
  }
  reported_connected_ = st.connected;
  reported_other_ = st.other_server;
  reported_refused_ = st.pair_refused;
}

std::string Agent::OpenPairing() {
  Channel c;
  Hello h;
  std::string err;
  int seconds = 0;
  if (cred_.password.empty()) return "Not paired yet";
  if (!c.Connect(s_.ip, s_.port, 2000, &err) || !c.SayHello(&h, &err) ||
      !c.OpenPairing(cred_.password, &seconds, &err)) {
    Log("Pair another PC: failed, %s", err.c_str());
    return "Pairing not opened: " + err;
  }
  Log("Pair another PC: the server accepts one more PC for %d s", seconds);
  return "Another PC can pair for the next " + std::to_string(seconds / 60) +
         " minutes: install ProsperoAutoDrive there.";
}

std::string Agent::QuitServer() {
  Channel c;
  Hello h;
  std::string err;
  if (cred_.password.empty()) return "Not paired yet";
  if (!c.Connect(s_.ip, s_.port, 2000, &err) || !c.SayHello(&h, &err) || !c.Quit(cred_.password, &err)) {
    Log("Shut down server: failed, %s", err.c_str());
    return "Server not shut down: " + err;
  }
  // The drive goes once the server's GOODBYE and close arrive.
  Log("Shut down server: requested, the server agreed");
  return "";
}

}  // namespace prospero
