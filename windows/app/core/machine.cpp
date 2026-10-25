// Prospero AutoDrive - connection state machine (phase 3, P3.1)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT

#include "machine.h"

#include <algorithm>
#include <utility>

namespace prospero::core {

namespace {

Action Do(Action::Kind k) { return Action{k}; }

Action Tell(Note n, std::string text = "") {
  Action a{Action::kNotify};
  a.note = n;
  a.text = std::move(text);
  return a;
}

}  // namespace

Machine::Machine(const Timing &t, std::string saved_server_id, bool have_credentials)
    : t_(t), server_id_(std::move(saved_server_id)), have_credentials_(have_credentials) {}

std::vector<Action> Machine::Start(int64_t now) {
  state_ = State::kConnecting;
  wake_ = -1;
  attempt_ = now;
  return {Do(Action::kConnectChannel)};
}

void Machine::Search(int64_t at) {
  state_ = State::kSearching;
  wake_ = at;
  ping_open_ = false;
  misses_ = 0;
}

// Each problem once until the next successful connect: a wrong password must
// not bring a notification every minute.
void Machine::Problem(std::vector<Action> *out, Note n, const std::string &text) {
  if (std::find(shown_.begin(), shown_.end(), n) != shown_.end()) return;
  shown_.push_back(n);
  out->push_back(Tell(n, text));
}

// The server is gone: drive away, back to searching.
void Machine::Lost(std::vector<Action> *out, int64_t now) {
  out->push_back(Do(Action::kCloseChannel));
  if (mapped_) {
    out->push_back(Do(Action::kUnmap));
    mapped_ = false;
  }
  if (announced_) {
    out->push_back(Tell(Note::kDisconnected));
    announced_ = false;
  }
  Search(now + t_.retry_ms);
}

std::vector<Action> Machine::Handle(const Event &e, int64_t now) {
  std::vector<Action> out;
  if (state_ == State::kStopped) return out;

  if (e.kind == Event::kShutdown) {
    out.push_back(Do(Action::kCloseChannel));
    if (mapped_) out.push_back(Do(Action::kUnmap));
    mapped_ = false;
    state_ = State::kStopped;
    wake_ = -1;
    return out;
  }
  if (e.kind == Event::kSearch) {
    out.push_back(Do(Action::kBroadcast));
    return out;
  }
  if (e.kind == Event::kAdopt) {
    // Only reached while searching: the other server ends every attempt.
    // Its credentials come from pairing, unless the PC knows it already.
    if (other_.empty()) return out;
    server_id_ = other_;
    other_.clear();
    have_credentials_ = false;
    shown_.clear();
    if (state_ == State::kSearching) {
      state_ = State::kConnecting;
      wake_ = -1;
      attempt_ = now;
      out.push_back(Do(Action::kConnectChannel));
    }
    return out;
  }

  switch (state_) {
    case State::kSearching:
      if ((e.kind == Event::kTick && now >= wake_) || e.kind == Event::kNetworkChanged ||
          e.kind == Event::kFound) {
        state_ = State::kConnecting;
        wake_ = -1;
        attempt_ = now;
        out.push_back(Do(Action::kConnectChannel));
      }
      break;

    case State::kConnecting:
    case State::kReconnecting:
      if (state_ == State::kReconnecting && e.kind == Event::kTick) {
        if (!in_flight_ && wake_ >= 0 && now >= wake_) {
          in_flight_ = true;
          wake_ = -1;
          out.push_back(Do(Action::kConnectChannel));
        }
        break;
      }
      if (e.kind == Event::kChannelFailed || e.kind == Event::kChannelClosed) {
        if (state_ == State::kReconnecting) {
          // The share answered a moment ago; ask again before giving up.
          in_flight_ = false;
          retried_ = true;
          state_ = State::kChecking;
          wake_ = -1;
          out.push_back(Do(Action::kProbe445));
        } else {
          // retry_ms from the start of the attempt, not from its timeout, so
          // the drive comes within retry_ms plus one connect.
          Search(std::max(now, attempt_ + t_.retry_ms));
        }
      } else if (e.kind == Event::kChannelUp) {
        if (!server_id_.empty() && e.server_id != server_id_) {
          other_ = e.server_id;
          Problem(&out, Note::kOtherServer, e.server_id);
          out.push_back(Do(Action::kCloseChannel));
          if (state_ == State::kReconnecting) {
            Lost(&out, now);
          } else {
            Search(now + t_.retry_ms);
          }
          break;
        }
        server_id_ = e.server_id;
        other_.clear();
        link_ = e.link.empty() ? "unknown" : e.link;
        if (e.ok) have_credentials_ = true;
        if (state_ == State::kReconnecting) {
          in_flight_ = false;
          retried_ = false;
          state_ = State::kOnline;
          ping_open_ = false;
          misses_ = 0;
          wake_ = now + t_.ping_ms;
        } else if (!have_credentials_) {
          state_ = State::kPairing;
          wake_ = -1;
          out.push_back(Do(Action::kPair));
        } else {
          state_ = State::kMapping;
          wake_ = -1;
          out.push_back(Do(Action::kMap));
        }
      }
      break;

    case State::kPairing:
      if (e.kind == Event::kPaired && e.ok) {
        have_credentials_ = true;
        state_ = State::kMapping;
        out.push_back(Do(Action::kMap));
      } else if (e.kind == Event::kPaired || e.kind == Event::kChannelClosed) {
        if (e.kind == Event::kPaired) Problem(&out, Note::kPairRefused);
        out.push_back(Do(Action::kCloseChannel));
        Search(now + (e.kind == Event::kPaired ? t_.backoff_ms : t_.retry_ms));
      }
      break;

    case State::kMapping:
      if (e.kind == Event::kMapped) {
        mapped_ = true;
        shown_.clear();
        if (e.reason == Event::kLetterBusy) Problem(&out, Note::kLetterBusy, e.text);
        if (!announced_) {
          out.push_back(Tell(Note::kConnected, e.text));
          announced_ = true;
        }
        state_ = State::kOnline;
        ping_open_ = false;
        misses_ = 0;
        wake_ = now + t_.ping_ms;
      } else if (e.kind == Event::kMapFailed) {
        int64_t again = t_.retry_ms;
        if (e.reason == Event::kAuth) {
          Problem(&out, Note::kLoginRefused);
          again = t_.backoff_ms;
        } else if (e.reason == Event::kNoLetter) {
          Problem(&out, Note::kNoLetter);
          again = t_.backoff_ms;
        } else {
          Problem(&out, Note::kMapFailed, e.text);
        }
        out.push_back(Do(Action::kCloseChannel));
        Search(now + again);
      } else if (e.kind == Event::kChannelClosed) {
        // The mapping answer still comes; the server is probably gone.
        out.push_back(Do(Action::kCloseChannel));
      }
      break;

    case State::kOnline:
      if (e.kind == Event::kPong) {
        ping_open_ = false;
        misses_ = 0;
      } else if (e.kind == Event::kTick && now >= wake_) {
        if (ping_open_ && ++misses_ >= MissLimit()) {
          state_ = State::kChecking;
          retried_ = false;
          wake_ = -1;
          out.push_back(Do(Action::kProbe445));
          break;
        }
        ping_open_ = true;
        out.push_back(Do(Action::kSendPing));
        wake_ += t_.ping_ms;
        if (wake_ <= now) wake_ = now + t_.ping_ms;    // after a long sleep
      } else if (e.kind == Event::kChannelClosed || e.kind == Event::kNetworkChanged) {
        state_ = State::kChecking;
        retried_ = false;
        wake_ = -1;
        out.push_back(Do(Action::kProbe445));
      }
      break;

    case State::kChecking:
      if (e.kind == Event::kPort445) {
        if (e.ok) {
          // The share is there, only the channel is not: build a new one,
          // at once the first time, then every retry_ms.
          out.push_back(Do(Action::kCloseChannel));
          state_ = State::kReconnecting;
          if (retried_) {
            in_flight_ = false;
            wake_ = now + t_.retry_ms;
          } else {
            in_flight_ = true;
            wake_ = -1;
            out.push_back(Do(Action::kConnectChannel));
          }
        } else {
          Lost(&out, now);
        }
      }
      break;

    case State::kStopped:
      break;
  }
  return out;
}

}  // namespace prospero::core
