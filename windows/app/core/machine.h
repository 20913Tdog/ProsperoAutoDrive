// Prospero AutoDrive - connection state machine (phase 3, P3.1)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// Decides when to connect, map, ping, check and unmap; it does no I/O
// itself and has no clock. The Win32 layer feeds it events with the time
// in ms, carries out the actions it returns, and sleeps until NextWakeMs()
// or the next event, so nothing runs in between (docs/entwurf-phase3.md).
// It knows a server only as "service channel plus SMB share".

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prospero::core {

struct Timing {
  int64_t retry_ms = 2000;          // connection attempts while searching
  int64_t ping_ms = 1000;           // heartbeat while online
  int misses_wired = 3;             // unanswered PINGs before the 445 check
  int misses_other = 12;            //   on wifi or unknown links
  int64_t backoff_ms = 60000;       // after refused credentials or pairing
};

enum class State {
  kSearching,      // no channel; tries the saved address every retry_ms
  kConnecting,     // channel attempt in flight
  kPairing,        // channel up, no credentials yet: PAIR sent
  kMapping,        // drive mapping in flight
  kOnline,         // drive mapped, PING every ping_ms
  kChecking,       // heartbeat lost or channel closed: TCP 445 test in flight
  kReconnecting,   // 445 answered: drive stays, new channel attempt
  kStopped,        // after Shutdown
};

enum class Note {
  kConnected,        // text: drive letter
  kDisconnected,
  kOtherServer,      // the address answers with another server ID
  kPairRefused,
  kLoginRefused,
  kLetterBusy,       // text: the letter used instead
  kNoLetter,         // every letter taken
  kMapFailed,        // text: reason
};

struct Action {
  enum Kind {
    kConnectChannel,   // to the saved address; answer ChannelUp / ChannelFailed
    kCloseChannel,
    kPair,             // answer Paired
    kMap,              // answer Mapped / MapFailed
    kUnmap,
    kSendPing,         // answer Pong (or nothing)
    kProbe445,         // answer Port445
    kBroadcast,        // search the network (only on request)
    kNotify,
  };
  Action(Kind k = kNotify) : kind(k) {}
  Kind kind;
  Note note = Note::kConnected;
  std::string text;
  bool operator==(const Action &o) const { return kind == o.kind && (kind != kNotify || note == o.note); }
};

struct Event {
  enum Kind {
    kTick,              // NextWakeMs() reached
    kChannelUp,         // server_id, link ("wired", "wifi", "unknown"); ok:
                        // credentials for this server are stored already
    kChannelFailed,
    kChannelClosed,     // the server closed an open channel
    kPaired,            // ok
    kMapped,            // text: drive letter
    kMapFailed,         // reason: kAuth, kLetterBusy, kNoLetter, kOther; text
    kPong,
    kPort445,           // ok
    kNetworkChanged,    // network change or resume from standby
    kSearch,            // the user asked for a network search
    kFound,             // the search found our server
    kAdopt,             // the user confirmed: take the other server instead
    kShutdown,
  };
  enum Reason { kAuth, kLetterBusy, kNoLetter, kOther };
  Event(Kind k = kTick) : kind(k) {}
  Kind kind;
  bool ok = false;
  std::string server_id;
  std::string link;
  std::string text;
  Reason reason = kOther;
};

class Machine {
 public:
  // saved_server_id empty: the first server that answers is taken (setup);
  // have_credentials: for that saved server.
  Machine(const Timing &t, std::string saved_server_id, bool have_credentials);

  std::vector<Action> Start(int64_t now);
  std::vector<Action> Handle(const Event &e, int64_t now);

  // When to send the next Tick; -1 while only an answer can change things.
  int64_t NextWakeMs() const { return wake_; }
  State state() const { return state_; }
  bool mapped() const { return mapped_; }
  const std::string &server_id() const { return server_id_; }
  const std::string &link() const { return link_; }
  // The ID of another server that answered at the saved address; empty
  // when there is none or ours answered since. Adopt takes it.
  const std::string &other_server() const { return other_; }

 private:
  void Search(int64_t at);
  void Lost(std::vector<Action> *out, int64_t now);
  void Problem(std::vector<Action> *out, Note n, const std::string &text = "");
  int MissLimit() const { return link_ == "wired" ? t_.misses_wired : t_.misses_other; }

  Timing t_;
  std::string server_id_;
  bool have_credentials_;
  State state_ = State::kSearching;
  int64_t wake_ = -1;
  int64_t attempt_ = 0;             // start of the last channel attempt
  bool mapped_ = false;
  bool announced_ = false;          // "connected" was shown, "disconnected" is due
  bool ping_open_ = false;          // a PING has no answer yet
  int misses_ = 0;
  bool in_flight_ = false;          // kReconnecting: channel attempt running
  bool retried_ = false;            // kReconnecting failed once since Online
  std::string link_ = "unknown";
  std::string other_;
  std::vector<Note> shown_;         // problems shown since the last connect
};

}  // namespace prospero::core
