// Prospero AutoDrive - client side of discovery and service channel
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// Generic network client for a file server that announces itself over UDP
// and keeps a line-based service channel over TCP (docs/entwurf-phase2.md).
// Used by the diagnosis tool prospero-probe and later by
// ProsperoAutoDrive.exe. No output here; callers decide what to show.
// Callers run WSAStartup() first.

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace prospero {

constexpr unsigned short kServicePort = 4445;     // TCP channel and UDP discovery

// UTC in ms since 1970, from the PC clock.
int64_t UtcMs();

// n random bytes as lowercase hex.
std::string RandomHex(int n);

// HMAC-SHA256 of msg under key, lowercase hex (Windows CNG).
std::string HmacSha256Hex(const std::string &key, const std::string &msg);

// "key=value" fields of a reply line after its first two words. Values
// never contain spaces.
std::map<std::string, std::string> Fields(const std::string &line);

std::string Ip(const in_addr &a);

// This PC's host name, reduced to letters, digits, '.', '_' and '-'.
std::string PcName();

// Discovery --------------------------------------------------------------

struct Here {
  std::string ip;              // where the reply came from
  std::string id;
  std::string name;
  unsigned short service_port = 0;
  unsigned short smb_port = 0;
  int64_t rtt_ms = 0;
  std::string line;
};

// Directed broadcast address of every active IPv4 interface, plus
// 255.255.255.255. Each entry also gets a description for diagnosis.
std::vector<std::pair<in_addr, std::string>> BroadcastTargets();

// Sends one request to every target and collects the matching replies for
// wait_ms. The socket is not bound explicitly, so no firewall prompt.
// Returns false only on a setup error (err set).
bool Discover(const std::vector<in_addr> &targets, unsigned short port, DWORD wait_ms,
              std::vector<Here> *found, std::string *err);

// Service channel --------------------------------------------------------

struct Hello {
  std::string server_id;
  std::string name;
  std::string share;
  unsigned short smb_port = 0;
  bool pairing_open = false;
  std::string line;
};

struct Pong {
  int64_t server_utc_ms = 0;
  int64_t skew_ms = 0;         // server minus PC, including the one-way delay
  bool trusted = false;
  int64_t rtt_ms = 0;
  std::string line;
};

class Channel {
 public:
  Channel() = default;
  ~Channel();
  Channel(const Channel &) = delete;
  Channel &operator=(const Channel &) = delete;

  bool Connect(const std::string &ip, unsigned short port, DWORD timeout_ms, std::string *err);
  void Close();

  // Sends one line and reads one reply line.
  bool Request(const std::string &line, std::string *reply, std::string *err);

  // Must come first: the nonce in its reply keys every mac= afterwards.
  bool SayHello(Hello *out, std::string *err);

  // Returns true with user and password, or false with refused set to the
  // server's reason (or err on a network error).
  bool Pair(const std::string &pc, std::string *user, std::string *password,
            std::string *refused, std::string *err);

  // With an empty key the PING carries no mac: a plain liveness check.
  bool Ping(const std::string &key, Pong *out, std::string *err);

  // Asks a server this PC is paired with to accept another PC for a while.
  bool OpenPairing(const std::string &key, int *seconds, std::string *err);

 private:
  // "<command> 1 seq=<n> <rest> mac=<hmac>"
  std::string Signed(const std::string &command, const std::string &rest, const std::string &key);
  bool ReadLine(std::string *line, std::string *err);

  SOCKET s_ = INVALID_SOCKET;
  std::string buf_;
  std::string nonce_;
  uint64_t seq_ = 0;           // grows with every signed line
};

// Credentials, in the Windows Credential Manager -------------------------

struct Credential {
  std::string user;
  std::string password;
  std::string last_ip;
};

bool LoadCredential(const std::string &server_id, Credential *out);
bool StoreCredential(const std::string &server_id, const Credential &c, std::string *err);

}  // namespace prospero
