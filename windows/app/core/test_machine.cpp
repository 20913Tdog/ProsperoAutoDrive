// Prospero AutoDrive - unit tests of the connection state machine (E1)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// A simulated clock and a simulated server: each action of the machine is
// answered after a delay, as the Win32 layer would. The times checked are
// the decision of 24.10.2026: drive within 3 s, gone within 5 s on the
// cable, 15 s otherwise, under 1 s when the server ends in order.

#include "machine.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using prospero::core::Action;
using prospero::core::Event;
using prospero::core::Machine;
using prospero::core::Note;
using prospero::core::State;
using prospero::core::Timing;

namespace {

int g_failed = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      g_failed++;                                                                \
    }                                                                            \
  } while (0)

// What the console does right now.
struct World {
  bool host_up = true;        // answers at all (off: every attempt times out)
  bool server_up = true;      // the payload runs (host up, payload off: refused)
  bool share_up = true;       // TCP 445 answers (same process, normally = server_up)
  bool silent = false;        // up, but packets are lost (Wi-Fi dropout)
  std::string id = "abc123";
  std::string link = "wired";
  bool login_ok = true;
  bool pair_ok = true;
};

class Sim {
 public:
  Sim(const std::string &saved_id, bool creds, Timing t = Timing())
      : m_(t, saved_id, creds) {}

  // Runs until `until` ms, answering actions and sending Ticks. The first
  // call starts the machine, so a test sets up the world before.
  void RunTo(int64_t until) {
    if (!started_) {
      started_ = true;
      Run(m_.Start(now_));
    }
    while (true) {
      int64_t next = until + 1;
      auto it = queue_.begin();
      if (it != queue_.end()) next = it->first;
      int64_t wake = m_.NextWakeMs();
      bool tick = wake >= 0 && wake <= next && wake <= until;
      if (!tick && next > until) break;
      if (tick) {
        now_ = wake < now_ ? now_ : wake;
        ticks_++;
        Run(m_.Handle(Event{Event::kTick}, now_));
        continue;
      }
      Event e = it->second;
      queue_.erase(it);
      now_ = next;
      Run(m_.Handle(e, now_));
    }
    now_ = until;
  }

  World w;
  std::vector<std::pair<int64_t, Action>> log;
  std::vector<std::pair<int64_t, bool>> drive;      // time, mapped
  int ticks_ = 0;
  int64_t now_ = 0;

  int Count(Action::Kind k) const {
    int n = 0;
    for (auto &a : log) n += a.second.kind == k;
    return n;
  }
  int Notes(Note n) const {
    int c = 0;
    for (auto &a : log) c += a.second.kind == Action::kNotify && a.second.note == n;
    return c;
  }
  // Time of the first change of the drive to `mapped` after `from`, -1 if none.
  int64_t DriveAt(bool mapped, int64_t from) const {
    for (auto &d : drive)
      if (d.first >= from && d.second == mapped) return d.first;
    return -1;
  }
  Machine &m() { return m_; }
  void Send(Event e, int64_t delay = 0) { queue_.emplace(now_ + delay, e); }

 private:
  void Run(const std::vector<Action> &acts) {
    for (const Action &a : acts) {
      log.emplace_back(now_, a);
      Answer(a);
    }
  }

  void Answer(const Action &a) {
    Event e;
    switch (a.kind) {
      case Action::kConnectChannel:
        if (!w.host_up || w.silent) {
          e.kind = Event::kChannelFailed;
          Send(e, 1000);                          // connect timeout
        } else if (!w.server_up) {
          e.kind = Event::kChannelFailed;
          Send(e, 1);                             // refused at once
        } else {
          e.kind = Event::kChannelUp;
          e.server_id = w.id;
          e.link = w.link;
          Send(e, 5);
          channel_ = true;
        }
        break;
      case Action::kCloseChannel:
        channel_ = false;
        break;
      case Action::kPair:
        e.kind = Event::kPaired;
        e.ok = w.pair_ok;
        Send(e, 5);
        break;
      case Action::kMap:
        if (w.login_ok) {
          e.kind = Event::kMapped;
          e.text = "P:";
          Send(e, 100);
        } else {
          e.kind = Event::kMapFailed;
          e.reason = Event::kAuth;
          Send(e, 100);
        }
        break;
      case Action::kUnmap:
        drive.emplace_back(now_, false);
        break;
      case Action::kSendPing:
        if (channel_ && w.server_up && w.host_up && !w.silent) {
          e.kind = Event::kPong;
          Send(e, 1);
        }
        break;
      case Action::kProbe445:
        e.kind = Event::kPort445;
        if (!w.host_up || w.silent) {
          e.ok = false;
          Send(e, 1000);
        } else {
          e.ok = w.share_up;
          Send(e, 1);
        }
        break;
      case Action::kBroadcast:
        break;
      case Action::kNotify:
        if (a.note == Note::kConnected) drive.emplace_back(now_, true);
        break;
    }
  }

  Machine m_;
  bool started_ = false;
  std::multimap<int64_t, Event> queue_;
  bool channel_ = false;
};

// The server stops: the channel closes now (orderly) or goes quiet.
void ServerEnds(Sim &s, bool orderly) {
  if (orderly) {
    s.w.server_up = false;
    s.w.share_up = false;
    Event e{Event::kChannelClosed};
    s.Send(e, 1);
  } else {
    s.w.host_up = false;
  }
}

void TestFirstConnect() {
  std::printf("first connect with pairing\n");
  Sim s("", false);
  s.RunTo(3000);
  CHECK(s.Count(Action::kPair) == 1);
  CHECK(s.m().mapped());
  CHECK(s.m().server_id() == "abc123");
  CHECK(s.DriveAt(true, 0) >= 0 && s.DriveAt(true, 0) <= 3000);
  CHECK(s.Notes(Note::kConnected) == 1);
}

void TestAppears() {
  std::printf("console comes on while searching: drive within 3 s, at any moment\n");
  int64_t worst = 0;
  for (int64_t on = 10000; on < 12000; on += 50) {
    Sim s("abc123", true);
    s.w.host_up = false;
    s.RunTo(on);
    CHECK(!s.m().mapped());
    s.w.host_up = true;
    s.RunTo(on + 10000);
    int64_t t = s.DriveAt(true, on);
    CHECK(t >= on);
    if (t - on > worst) worst = t - on;
  }
  std::printf("  drive after at most %lld ms\n", static_cast<long long>(worst));
  CHECK(worst <= 3000);
}

void TestGone(const char *link, bool orderly, int64_t max_ms, int64_t min_ms) {
  std::printf("server ends (%s, %s): drive gone within %lld ms\n", link,
              orderly ? "orderly" : "silent", static_cast<long long>(max_ms));
  Sim s("abc123", true);
  s.w.link = link;
  s.RunTo(5000);
  CHECK(s.m().mapped());
  ServerEnds(s, orderly);
  s.RunTo(40000);
  int64_t t = s.DriveAt(false, 5000);
  std::printf("  drive gone after %lld ms\n", static_cast<long long>(t - 5000));
  CHECK(t >= 0);
  CHECK(t - 5000 <= max_ms);
  CHECK(t - 5000 >= min_ms);
  CHECK(s.Notes(Note::kDisconnected) == 1);
  CHECK(s.m().state() == State::kSearching || s.m().state() == State::kConnecting);
}

void TestDropout(const char *link, int64_t ms) {
  std::printf("dropout of %lld ms on %s: drive stays, nothing shown\n",
              static_cast<long long>(ms), link);
  Sim s("abc123", true);
  s.w.link = link;
  s.RunTo(5000);
  s.w.silent = true;
  s.RunTo(5000 + ms);
  s.w.silent = false;
  s.RunTo(60000);
  CHECK(s.m().mapped());
  CHECK(s.Count(Action::kUnmap) == 0);
  CHECK(s.Notes(Note::kDisconnected) == 0);
  CHECK(s.Notes(Note::kConnected) == 1);
}

void TestWrongPassword() {
  std::printf("refused login: shown once, one try per minute\n");
  Sim s("abc123", true);
  s.w.login_ok = false;
  s.RunTo(600000);
  CHECK(!s.m().mapped());
  CHECK(s.Notes(Note::kLoginRefused) == 1);
  int maps = s.Count(Action::kMap);
  std::printf("  %d tries in 10 min\n", maps);
  CHECK(maps >= 9 && maps <= 11);
}

void TestOtherServer() {
  std::printf("another server at the address: shown once, no drive\n");
  Sim s("abc123", true);
  s.w.id = "other";
  s.RunTo(60000);
  CHECK(!s.m().mapped());
  CHECK(s.Count(Action::kMap) == 0);
  CHECK(s.Notes(Note::kOtherServer) == 1);
  CHECK(s.m().other_server() == "other");
}

void TestAdopt() {
  std::printf("server set up anew: offered, taken only on request, then paired\n");
  Sim s("abc123", true);
  Event adopt{Event::kAdopt};
  s.Send(adopt);              // nothing to take yet
  s.RunTo(5000);
  CHECK(s.m().mapped());
  CHECK(s.m().other_server().empty());
  // It ends in order and comes back with a new ID (state folder deleted).
  ServerEnds(s, true);
  s.RunTo(6000);
  s.w.server_up = true;
  s.w.share_up = true;
  s.w.id = "fresh";
  s.RunTo(20000);
  CHECK(!s.m().mapped());
  CHECK(s.m().other_server() == "fresh");
  CHECK(s.Count(Action::kPair) == 0);
  CHECK(s.Notes(Note::kOtherServer) == 1);
  s.Send(adopt);
  s.RunTo(23000);
  CHECK(s.m().server_id() == "fresh");
  CHECK(s.m().other_server().empty());
  CHECK(s.Count(Action::kPair) == 1);
  CHECK(s.m().mapped());
  int64_t t = s.DriveAt(true, 20000);
  std::printf("  drive %lld ms after the confirmation\n", static_cast<long long>(t - 20000));
  CHECK(t >= 20000 && t - 20000 <= 3000);
}

void TestChannelOnly() {
  std::printf("channel lost, share still there: drive stays, no busy loop\n");
  Sim s("abc123", true);
  s.RunTo(5000);
  // The service channel refuses, SMB still answers.
  s.w.server_up = false;
  s.w.share_up = true;
  Event e{Event::kChannelClosed};
  s.Send(e, 1);
  s.RunTo(25000);
  CHECK(s.m().mapped());
  int connects = s.Count(Action::kConnectChannel);
  std::printf("  %d channel attempts in 20 s\n", connects);
  CHECK(connects <= 13);
  s.w.server_up = true;
  s.RunTo(30000);
  CHECK(s.m().state() == State::kOnline);
  CHECK(s.Notes(Note::kDisconnected) == 0);
}

void TestQuiet() {
  std::printf("idle: at most one wake-up a second online, one in 2 s searching\n");
  Sim s("abc123", true);
  s.RunTo(1000);
  int t0 = s.ticks_;
  s.RunTo(61000);
  int online = s.ticks_ - t0;
  std::printf("  %d wake-ups in 60 s online\n", online);
  CHECK(online <= 61);
  s.w.host_up = false;
  s.RunTo(80000);
  t0 = s.ticks_;
  s.RunTo(140000);
  int searching = s.ticks_ - t0;
  std::printf("  %d wake-ups in 60 s searching\n", searching);
  CHECK(searching <= 31);
}

void TestNetworkChange() {
  std::printf("network change while searching: tries at once\n");
  Sim s("abc123", true);
  s.w.host_up = false;
  s.RunTo(11500);          // searching, next attempt at 12000
  CHECK(s.m().state() == State::kSearching);
  s.w.host_up = true;
  Event e{Event::kNetworkChanged};
  s.Send(e);
  s.RunTo(11700);
  CHECK(s.m().mapped());
}

void TestShutdown() {
  std::printf("shutdown: drive removed\n");
  Sim s("abc123", true);
  s.RunTo(3000);
  Event e{Event::kShutdown};
  s.Send(e);
  s.RunTo(3100);
  CHECK(!s.m().mapped());
  CHECK(s.Count(Action::kUnmap) == 1);
  CHECK(s.m().state() == State::kStopped);
  CHECK(s.m().NextWakeMs() < 0);
}

void TestSearchOnRequest() {
  std::printf("broadcast only on request\n");
  Sim s("abc123", true);
  s.w.host_up = false;
  s.RunTo(300000);
  CHECK(s.Count(Action::kBroadcast) == 0);
  Event e{Event::kSearch};
  s.Send(e);
  s.RunTo(300100);
  CHECK(s.Count(Action::kBroadcast) == 1);
}

}  // namespace

int main() {
  TestFirstConnect();
  TestAppears();
  TestGone("wired", false, 5000, 3000);
  TestGone("wifi", false, 15000, 10000);
  TestGone("unknown", false, 15000, 10000);
  TestGone("wired", true, 1000, 0);
  TestGone("wifi", true, 1000, 0);
  TestDropout("wired", 2000);
  TestDropout("wifi", 9000);
  TestWrongPassword();
  TestOtherServer();
  TestAdopt();
  TestChannelOnly();
  TestQuiet();
  TestNetworkChange();
  TestShutdown();
  TestSearchOnRequest();
  if (g_failed) {
    std::printf("MACHINE-TEST: %d check(s) FAILED\n", g_failed);
    return 1;
  }
  std::printf("MACHINE-TEST: OK\n");
  return 0;
}
