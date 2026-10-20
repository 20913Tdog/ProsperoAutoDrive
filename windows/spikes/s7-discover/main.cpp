// Prospero AutoDrive - Spike S7: find the server by UDP
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// Question: can a program running as a standard user find the server on
// the local network, by unicast and by broadcast, without Windows showing
// a firewall prompt? The request carries the PC's UTC time, so the server
// can measure how far its clock is off (decision "variant 1").
//
// The socket is never bound explicitly: the first sendto() binds it to an
// ephemeral port, as any client does, and only replies to our own
// requests come back to it.
//
//   prospero-s7-discover.exe [IPv4 address] [--port N] [--unicast-only]
//
// --unicast-only skips the broadcast, for the CI and for local tests.
//
// Exit code: 0 all expected replies, +1 no unicast reply (only with an
// address), +2 no broadcast reply, 3 on a setup error without address.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr unsigned short kDefaultPort = 4446;
constexpr int kTries = 3;
constexpr DWORD kWaitMs = 1500;

int64_t UtcMs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER t;
  t.LowPart = ft.dwLowDateTime;
  t.HighPart = ft.dwHighDateTime;
  // FILETIME counts 100 ns since 1601-01-01.
  return static_cast<int64_t>((t.QuadPart - 116444736000000000ULL) / 10000);
}

std::string Nonce() {
  unsigned char raw[8];
  if (BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    // Not secret, only tells our replies apart.
    for (auto &b : raw) b = static_cast<unsigned char>(GetTickCount64() * 31 + (&b - raw));
  }
  static const char hex[] = "0123456789abcdef";
  std::string s;
  for (unsigned char b : raw) {
    s += hex[b >> 4];
    s += hex[b & 15];
  }
  return s;
}

std::string Ip(const sockaddr_in &a) {
  char buf[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
  return buf;
}

// Directed broadcast address of every active IPv4 interface, plus the
// limited broadcast 255.255.255.255.
std::vector<in_addr> BroadcastTargets() {
  std::vector<in_addr> out;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf(size);
  auto *aa = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    aa = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
    rc = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size);
  }
  if (rc == NO_ERROR) {
    for (auto *a = aa; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
        auto *sin = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
        ULONG mask = 0;
        if (ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask) != NO_ERROR) continue;
        in_addr b;
        b.s_addr = sin->sin_addr.s_addr | ~mask;
        std::printf("  Netz: %ls, Adresse %s/%u, Broadcast ", a->FriendlyName,
                    Ip(*sin).c_str(), static_cast<unsigned>(u->OnLinkPrefixLength));
        sockaddr_in tmp{};
        tmp.sin_addr = b;
        std::printf("%s\n", Ip(tmp).c_str());
        out.push_back(b);
      }
    }
  } else {
    std::printf("  GetAdaptersAddresses fehlgeschlagen: %lu\n", rc);
  }
  in_addr all;
  all.s_addr = INADDR_BROADCAST;
  out.push_back(all);
  return out;
}

struct Result {
  int replies = 0;
  int64_t best_rtt = -1;
  std::string first;
};

// Sends one request to each target and collects matching replies until
// kWaitMs after the last send.
Result Probe(SOCKET s, const std::vector<in_addr> &targets, unsigned short port) {
  Result r;
  for (int t = 1; t <= kTries; t++) {
    std::string nonce = Nonce();
    int64_t sent = UtcMs();
    std::string req = "PROSPERO-DISCOVER 1 nonce=" + nonce + " utc_ms=" + std::to_string(sent) + "\n";
    for (const auto &a : targets) {
      sockaddr_in to{};
      to.sin_family = AF_INET;
      to.sin_port = htons(port);
      to.sin_addr = a;
      if (sendto(s, req.data(), static_cast<int>(req.size()), 0,
                 reinterpret_cast<sockaddr *>(&to), sizeof to) == SOCKET_ERROR) {
        std::printf("  Versuch %d: sendto %s fehlgeschlagen, WSA-Fehler %d\n", t,
                    Ip(to).c_str(), WSAGetLastError());
      }
    }
    ULONGLONG until = GetTickCount64() + kWaitMs;
    int got = 0;
    for (;;) {
      ULONGLONG now = GetTickCount64();
      if (now >= until) break;
      fd_set rd;
      FD_ZERO(&rd);
      FD_SET(s, &rd);
      timeval tv{0, static_cast<long>((until - now) * 1000)};
      if (select(0, &rd, nullptr, nullptr, &tv) <= 0) break;
      char buf[600];
      sockaddr_in from{};
      int flen = sizeof from;
      int n = recvfrom(s, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &flen);
      if (n <= 0) {
        int err = WSAGetLastError();
        // An ICMP "port unreachable" for an earlier datagram: nothing
        // listens there. Keep waiting for the other targets.
        if (err == WSAECONNRESET) {
          std::printf("  Versuch %d: ICMP meldet Port nicht erreichbar\n", t);
          continue;
        }
        std::printf("  Versuch %d: recvfrom WSA-Fehler %d\n", t, err);
        break;
      }
      buf[n] = '\0';
      while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
      std::string line(buf);
      if (line.rfind("PROSPERO-HERE 1 nonce=" + nonce + " ", 0) != 0) {
        std::printf("  Versuch %d: fremde Antwort von %s ignoriert\n", t, Ip(from).c_str());
        continue;
      }
      int64_t rtt = UtcMs() - sent;
      std::printf("  Versuch %d: Antwort von %s nach %lld ms: %s\n", t, Ip(from).c_str(),
                  static_cast<long long>(rtt), line.c_str());
      if (r.best_rtt < 0 || rtt < r.best_rtt) r.best_rtt = rtt;
      if (r.first.empty()) r.first = Ip(from) + "  " + line;
      r.replies++;
      got++;
    }
    if (!got) std::printf("  Versuch %d: keine Antwort\n", t);
  }
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  const char *ip = nullptr;
  unsigned short port = kDefaultPort;
  bool broadcast = true;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--unicast-only")) {
      broadcast = false;
    } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
      port = static_cast<unsigned short>(std::atoi(argv[++i]));
    } else {
      ip = argv[i];
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::printf("FEHLER: WSAStartup\n");
    return 3;
  }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    std::printf("FEHLER: socket, WSA-Fehler %d\n", WSAGetLastError());
    return 3;
  }
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on), sizeof on);

  std::printf("PC-Zeit UTC (ms): %lld, UDP-Port %u\n", static_cast<long long>(UtcMs()), port);
  int rc = 0;
  if (ip) {
    in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1) {
      std::printf("FEHLER: keine IPv4-Adresse: %s\n", ip);
      return 3;
    }
    std::printf("--- Unicast an %s\n", ip);
    Result u = Probe(s, {a}, port);
    if (u.replies) {
      std::printf("S7-UNICAST: OK, %d Antworten, schnellste %lld ms\n", u.replies,
                  static_cast<long long>(u.best_rtt));
    } else {
      std::printf("S7-UNICAST: KEINE ANTWORT\n");
      rc |= 1;
    }
  }
  if (broadcast) {
    std::printf("--- Broadcast\n");
    Result b = Probe(s, BroadcastTargets(), port);
    if (b.replies) {
      std::printf("S7-BROADCAST: OK, %d Antworten, schnellste %lld ms, erste: %s\n", b.replies,
                  static_cast<long long>(b.best_rtt), b.first.c_str());
    } else {
      std::printf("S7-BROADCAST: KEINE ANTWORT\n");
      rc |= 2;
    }
  }
  closesocket(s);
  WSACleanup();
  return rc;
}
