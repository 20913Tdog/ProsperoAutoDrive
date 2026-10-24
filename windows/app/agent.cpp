// Prospero AutoDrive - Win32 layer of ProsperoAutoDrive.exe (phase 3, P3.2)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT

#include "agent.h"

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
  NotifyIpInterfaceChange(AF_UNSPEC, OnNetChange, net_event_, FALSE, &net_notify_);
}

Agent::~Agent() {
  if (net_notify_) CancelMibChangeNotify2(net_notify_);
  ch_.Close();
  if (sock_event_) WSACloseEvent(sock_event_);
  if (net_event_) CloseHandle(net_event_);
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
  Log("Start: Server %s unter %s:%u", s_.server_id.empty() ? "(noch keiner)" : s_.server_id.c_str(),
      s_.ip.c_str(), s_.port);
  Report("", false);
  Do(m.Start(Now()));
  HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  for (;;) {
    HANDLE hs[4] = {stop, sock_event_, net_event_, search ? search : never};
    int64_t wake = m.NextWakeMs();
    DWORD to = INFINITE;
    if (wake >= 0) {
      int64_t d = wake - Now();
      to = d > 0 ? static_cast<DWORD>(d) : 0;
    }
    DWORD r = WaitForMultipleObjects(4, hs, FALSE, to);
    if (r == WAIT_OBJECT_0) {
      Log("Ende angefordert");
      Do(m.Handle(Event(Event::kShutdown), Now()));
      break;
    }
    if (r == WAIT_OBJECT_0 + 1) {
      WSAResetEvent(sock_event_);
      if (!ch_.open()) continue;
      bool closed = false;
      int pongs = ch_.Poll(&closed);
      if (pongs) last_pong_ = Now();
      for (int i = 0; i < pongs; i++) Do(m.Handle(Event(Event::kPong), Now()));
      if (closed) {
        Log("Dienstkanal vom Server geschlossen");
        ch_.Close();
        Do(m.Handle(Event(Event::kChannelClosed), Now()));
      }
    } else if (r == WAIT_OBJECT_0 + 2) {
      ResetEvent(net_event_);
      Do(m.Handle(Event(Event::kNetworkChanged), Now()));
    } else if (r == WAIT_OBJECT_0 + 3) {
      ResetEvent(search);
      Do(m.Handle(Event(Event::kSearch), Now()));
    } else if (r == WAIT_TIMEOUT) {
      Do(m.Handle(Event(Event::kTick), Now()));
    } else {
      Log("Warten fehlgeschlagen, Fehler %lu", GetLastError());
      Sleep(1000);
    }
  }
  CloseHandle(never);
  m_ = nullptr;
  Log("beendet");
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
          Log("gekoppelt, Zugangsdaten in der Anmeldeinformationsverwaltung");
        } else {
          Log("Zugangsdaten nicht gespeichert: %s", err.c_str());
        }
      } else {
        Log("Koppeln: %s", refused.empty() ? err.c_str() : refused.c_str());
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
        Log("PING: %s", err.c_str());
        ch_.Close();
        return Event(Event::kChannelClosed);
      }
      break;
    case Action::kProbe445: {
      Event e(Event::kPort445);
      unsigned short port = hello_.smb_port ? hello_.smb_port : 445;
      e.ok = Probe(port);
      // When the last PONG came tells how long the server has been gone.
      Log("Test Port %u: %s, letzte Antwort auf ein Lebenszeichen vor %lld ms", port,
          e.ok ? "antwortet" : "keine Antwort",
          static_cast<long long>(last_pong_ ? Now() - last_pong_ : -1));
      return e;
    }
    case Action::kBroadcast: {
      // Up to 30 s, one request every 2 s, until the server answers.
      Log("Suche im Netz");
      std::vector<in_addr> targets;
      for (auto &t : BroadcastTargets()) targets.push_back(t.first);
      for (int round = 0; round < 15; round++) {
        std::vector<Here> found;
        if (!Discover(targets, s_.port, 2000, &found, &err)) break;
        for (const Here &h : found) {
          if (s_.server_id.empty() || h.id == s_.server_id) {
            Log("gefunden: %s unter %s", h.id.c_str(), h.ip.c_str());
            s_.ip = h.ip;
            s_.Save();
            return Event(Event::kFound);
          }
        }
      }
      Log("Suche: nichts gefunden");
      Report(s_.name.empty() ? "Server nicht gefunden" : s_.name + " nicht gefunden", true);
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
    if (err != last_err) Log("Dienstkanal: %s", err.c_str());
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
  std::string up = hello_.server_id + " (" + hello_.name + "), Verbindung " +
                   (hello_.link.empty() ? "unbekannt" : hello_.link);
  if (up != last_up) Log("Dienstkanal: %s", up.c_str());
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
    e.text = "Windows-Fehler " + std::to_string(rc);
    if (rc != last_map_err_) {
      Log("%s -> %s: Windows-Fehler %lu", Narrow(chosen).c_str(), Narrow(remote).c_str(), rc);
    }
    last_map_err_ = rc;
    return e;
  }
  last_map_err_ = 0;
  letter_ = chosen;
  Log("%s -> %s verbunden in %lld ms", Narrow(chosen).c_str(), Narrow(remote).c_str(),
      static_cast<long long>(Now() - t0));
  if (s_.server_id != hello_.server_id || s_.name != hello_.name) {
    s_.server_id = hello_.server_id;
    s_.name = hello_.name;
    s_.Save();
  }
  SYSTEMTIME lt;
  GetLocalTime(&lt);
  char seen[40];
  snprintf(seen, sizeof seen, "%02u.%02u.%04u %02u:%02u", lt.wDay, lt.wMonth, lt.wYear, lt.wHour,
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
  Log("%s getrennt in %lld ms (%s)%s", Narrow(letter_).c_str(), static_cast<long long>(Now() - t0),
      how, rc == NO_ERROR ? "" : (", Windows-Fehler " + std::to_string(rc)).c_str());
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
      msg = who + " verbunden als " + text;
      problem = false;
      break;
    case core::Note::kDisconnected:
      msg = who + " getrennt";
      problem = false;
      break;
    case core::Note::kOtherServer:
      msg = "Unter " + s_.ip + " antwortet ein anderer Server (" + text + ")";
      break;
    case core::Note::kPairRefused:
      msg = who + " hat das Koppeln abgelehnt. Auf einem schon gekoppelten PC "
            "\"Weiteren PC koppeln\" waehlen.";
      break;
    case core::Note::kLoginRefused:
      msg = who + ": Anmeldung an der Freigabe abgelehnt";
      break;
    case core::Note::kLetterBusy:
      msg = "P: ist belegt, das Laufwerk heisst " + text;
      problem = false;
      break;
    case core::Note::kNoLetter:
      msg = "Kein Laufwerksbuchstabe frei";
      break;
    case core::Note::kMapFailed:
      msg = "Laufwerk nicht verbunden: " + text;
      break;
  }
  Log("MELDUNG: %s", msg.c_str());
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
  if (on_status && (!message.empty() || st.connected != reported_connected_ || !m_)) on_status(st);
  reported_connected_ = st.connected;
}

std::string Agent::OpenPairing() {
  Channel c;
  Hello h;
  std::string err;
  int seconds = 0;
  if (cred_.password.empty()) return "Noch nicht gekoppelt";
  if (!c.Connect(s_.ip, s_.port, 2000, &err) || !c.SayHello(&h, &err) ||
      !c.OpenPairing(cred_.password, &seconds, &err)) {
    Log("Weiteren PC koppeln: %s", err.c_str());
    return "Koppeln nicht geoeffnet: " + err;
  }
  Log("Weiteren PC koppeln: %d s offen", seconds);
  return "Ein weiterer PC kann sich jetzt " + std::to_string(seconds / 60) +
         " Minuten lang koppeln: dort ProsperoAutoDrive installieren.";
}

}  // namespace prospero
