// Prospero AutoDrive - client side of discovery and service channel
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT

#include "client.h"

#include <iphlpapi.h>
#include <bcrypt.h>
#include <wincred.h>

#include <cstdlib>
#include <cstring>

namespace prospero {

namespace {

constexpr wchar_t kCredPrefix[] = L"ProsperoAutoDrive/server/";

std::wstring Wide(const std::string &s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const wchar_t *w, size_t len) {
  if (!len) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
  return s;
}

std::string WsaError(const char *what) {
  return std::string(what) + ", WSA-Fehler " + std::to_string(WSAGetLastError());
}

}  // namespace

int64_t UtcMs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER t;
  t.LowPart = ft.dwLowDateTime;
  t.HighPart = ft.dwHighDateTime;
  // FILETIME counts 100 ns since 1601-01-01.
  return static_cast<int64_t>((t.QuadPart - 116444736000000000ULL) / 10000);
}

std::string RandomHex(int n) {
  std::vector<unsigned char> raw(static_cast<size_t>(n));
  if (BCryptGenRandom(nullptr, raw.data(), static_cast<ULONG>(n),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    // Only used for nonces and folder names, never for secrets.
    for (size_t i = 0; i < raw.size(); i++) {
      raw[i] = static_cast<unsigned char>(GetTickCount64() * 31 + i * 7);
    }
  }
  static const char hex[] = "0123456789abcdef";
  std::string s;
  for (unsigned char b : raw) {
    s += hex[b >> 4];
    s += hex[b & 15];
  }
  return s;
}

std::string HmacSha256Hex(const std::string &key, const std::string &msg) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  unsigned char mac[32];
  std::string out;

  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
    return out;
  }
  if (BCryptCreateHash(alg, &hash, nullptr, 0,
                       reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
                       static_cast<ULONG>(key.size()), 0) == 0 &&
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(msg.data())),
                     static_cast<ULONG>(msg.size()), 0) == 0 &&
      BCryptFinishHash(hash, mac, sizeof mac, 0) == 0) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned char b : mac) {
      out += hex[b >> 4];
      out += hex[b & 15];
    }
  }
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return out;
}

std::map<std::string, std::string> Fields(const std::string &line) {
  std::map<std::string, std::string> f;
  size_t pos = line.find(' ');
  if (pos != std::string::npos) pos = line.find(' ', pos + 1);
  while (pos != std::string::npos) {
    size_t start = pos + 1;
    pos = line.find(' ', start);
    std::string item = line.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
    size_t eq = item.find('=');
    if (eq != std::string::npos) f[item.substr(0, eq)] = item.substr(eq + 1);
  }
  return f;
}

std::string Ip(const in_addr &a) {
  char buf[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &a, buf, sizeof buf);
  return buf;
}

std::string PcName() {
  wchar_t w[256];
  DWORD n = 256;
  std::string out;
  if (GetComputerNameExW(ComputerNameDnsHostname, w, &n)) {
    for (char c : Narrow(w, n)) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == '-') {
        out += c;
      }
    }
  }
  if (out.size() > 63) out.resize(63);
  return out.empty() ? "pc" : out;
}

// Discovery --------------------------------------------------------------

std::vector<std::pair<in_addr, std::string>> BroadcastTargets() {
  std::vector<std::pair<in_addr, std::string>> out;
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
        std::string desc = Narrow(a->FriendlyName, wcslen(a->FriendlyName)) + ", " +
                           Ip(sin->sin_addr) + "/" + std::to_string(u->OnLinkPrefixLength);
        out.emplace_back(b, desc);
      }
    }
  }
  in_addr all;
  all.s_addr = INADDR_BROADCAST;
  out.emplace_back(all, "alle (255.255.255.255)");
  return out;
}

bool Discover(const std::vector<in_addr> &targets, unsigned short port, DWORD wait_ms,
              std::vector<Here> *found, std::string *err) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    *err = WsaError("socket");
    return false;
  }
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on), sizeof on);

  std::string nonce = RandomHex(8);
  std::string req = "PROSPERO-DISCOVER 1 nonce=" + nonce + "\n";
  std::string expect = "PROSPERO-HERE 1 nonce=" + nonce + " ";
  int64_t sent = UtcMs();
  for (const auto &a : targets) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr = a;
    sendto(s, req.data(), static_cast<int>(req.size()), 0, reinterpret_cast<sockaddr *>(&to),
           sizeof to);
  }
  ULONGLONG until = GetTickCount64() + wait_ms;
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
      // WSAECONNRESET: an ICMP "port unreachable" for one target. The
      // others may still answer.
      if (WSAGetLastError() == WSAECONNRESET) continue;
      break;
    }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    std::string line(buf, static_cast<size_t>(n));
    if (line.rfind(expect, 0) != 0) continue;
    auto f = Fields(line);
    Here h;
    h.ip = Ip(from.sin_addr);
    h.id = f["id"];
    h.name = f["name"];
    h.service_port = static_cast<unsigned short>(std::atoi(f["service_port"].c_str()));
    h.smb_port = static_cast<unsigned short>(std::atoi(f["smb_port"].c_str()));
    h.rtt_ms = UtcMs() - sent;
    h.line = line;
    found->push_back(h);
  }
  closesocket(s);
  return true;
}

// Service channel --------------------------------------------------------

Channel::~Channel() { Close(); }

void Channel::Close() {
  if (s_ != INVALID_SOCKET) closesocket(s_);
  s_ = INVALID_SOCKET;
  buf_.clear();
  nonce_.clear();
  seq_ = 0;
}

bool Channel::Connect(const std::string &ip, unsigned short port, DWORD timeout_ms,
                      std::string *err) {
  Close();
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) {
    *err = "keine IPv4-Adresse: " + ip;
    return false;
  }
  s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s_ == INVALID_SOCKET) {
    *err = WsaError("socket");
    return false;
  }
  // Non-blocking connect with a timeout, then blocking with timeouts.
  u_long nb = 1;
  ioctlsocket(s_, FIONBIO, &nb);
  connect(s_, reinterpret_cast<sockaddr *>(&to), sizeof to);
  fd_set wr, ex;
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  FD_SET(s_, &wr);
  FD_SET(s_, &ex);
  timeval tv{static_cast<long>(timeout_ms / 1000), static_cast<long>(timeout_ms % 1000 * 1000)};
  if (select(0, nullptr, &wr, &ex, &tv) <= 0 || !FD_ISSET(s_, &wr)) {
    int soerr = 0;
    int len = sizeof soerr;
    getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soerr), &len);
    *err = "keine Verbindung zu " + ip + ":" + std::to_string(port) +
           (soerr ? ", Fehler " + std::to_string(soerr) : ", Zeitlimit");
    Close();
    return false;
  }
  nb = 0;
  ioctlsocket(s_, FIONBIO, &nb);
  DWORD t = 5000;
  setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&t), sizeof t);
  setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&t), sizeof t);
  BOOL on = TRUE;
  setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
  return true;
}

bool Channel::ReadLine(std::string *line, std::string *err) {
  for (;;) {
    size_t nl = buf_.find('\n');
    if (nl != std::string::npos) {
      *line = buf_.substr(0, nl);
      if (!line->empty() && line->back() == '\r') line->pop_back();
      buf_.erase(0, nl + 1);
      return true;
    }
    char tmp[1024];
    int n = recv(s_, tmp, sizeof tmp, 0);
    if (n <= 0) {
      *err = n == 0 ? "Verbindung vom Server geschlossen" : WsaError("recv");
      return false;
    }
    buf_.append(tmp, static_cast<size_t>(n));
    if (buf_.size() > 64 * 1024) {
      *err = "Antwortzeile zu lang";
      return false;
    }
  }
}

bool Channel::Request(const std::string &line, std::string *reply, std::string *err) {
  if (s_ == INVALID_SOCKET) {
    *err = "nicht verbunden";
    return false;
  }
  std::string out = line + "\n";
  if (send(s_, out.data(), static_cast<int>(out.size()), 0) != static_cast<int>(out.size())) {
    *err = WsaError("send");
    return false;
  }
  return ReadLine(reply, err);
}

std::string Channel::Signed(const std::string &command, const std::string &rest,
                            const std::string &key) {
  std::string line = command + " 1 seq=" + std::to_string(++seq_) + (rest.empty() ? "" : " " + rest);
  return line + " mac=" + HmacSha256Hex(key, nonce_ + " " + line);
}

bool Channel::SayHello(Hello *out, std::string *err) {
  std::string rep;
  if (!Request("HELLO 1 pc=" + PcName(), &rep, err)) return false;
  if (rep.rfind("HELLO 1 ", 0) != 0) {
    *err = "unerwartete Antwort: " + rep;
    return false;
  }
  auto f = Fields(rep);
  out->server_id = f["server_id"];
  out->name = f["name"];
  out->share = f["share"];
  out->smb_port = static_cast<unsigned short>(std::atoi(f["smb_port"].c_str()));
  out->pairing_open = f["pairing"] == "open";
  out->line = rep;
  nonce_ = f["nonce"];
  if (out->server_id.empty() || out->share.empty() || nonce_.empty()) {
    *err = "unvollstaendige Antwort: " + rep;
    return false;
  }
  return true;
}

bool Channel::Pair(const std::string &pc, std::string *user, std::string *password,
                   std::string *refused, std::string *err) {
  std::string rep;
  if (!Request("PAIR 1 pc=" + pc, &rep, err)) return false;
  auto f = Fields(rep);
  if (rep.rfind("PAIRED 1 ", 0) == 0 && !f["user"].empty() && !f["password"].empty()) {
    *user = f["user"];
    *password = f["password"];
    return true;
  }
  *refused = rep.rfind("PAIR-REFUSED 1 ", 0) == 0 ? f["reason"] : "unerwartete Antwort: " + rep;
  return false;
}

bool Channel::Ping(const std::string &key, Pong *out, std::string *err) {
  int64_t sent = UtcMs();
  std::string time = "utc_ms=" + std::to_string(sent);
  std::string rep;
  if (!Request(key.empty() ? "PING 1 " + time : Signed("PING", time, key), &rep, err)) return false;
  if (rep.rfind("PONG 1 ", 0) != 0) {
    *err = "unerwartete Antwort: " + rep;
    return false;
  }
  auto f = Fields(rep);
  out->server_utc_ms = std::atoll(f["utc_ms"].c_str());
  out->skew_ms = std::atoll(f["skew_ms"].c_str());
  out->trusted = f["trusted"] == "1";
  out->rtt_ms = UtcMs() - sent;
  out->line = rep;
  return true;
}

bool Channel::OpenPairing(const std::string &key, int *seconds, std::string *err) {
  std::string rep;
  if (!Request(Signed("OPEN-PAIRING", "", key), &rep, err)) {
    return false;
  }
  if (rep.rfind("PAIRING-OPEN 1 ", 0) != 0) {
    *err = "abgelehnt: " + rep;
    return false;
  }
  *seconds = std::atoi(Fields(rep)["seconds"].c_str());
  return true;
}

bool Channel::Status(std::map<std::string, std::string> *out, std::string *err) {
  std::string line;
  if (!Request("STATUS 1", &line, err)) return false;
  if (line != "STATUS 1") {
    *err = "unerwartete Antwort: " + line;
    return false;
  }
  out->clear();
  for (;;) {
    if (!ReadLine(&line, err)) return false;
    if (line == "END 1") return true;
    size_t eq = line.find('=');
    if (eq != std::string::npos) (*out)[line.substr(0, eq)] = line.substr(eq + 1);
  }
}

bool Channel::ResetStats(std::string *err) {
  std::string rep;
  if (!Request("RESET-STATS 1", &rep, err)) return false;
  if (rep != "RESET-STATS 1") {
    *err = "unerwartete Antwort: " + rep;
    return false;
  }
  return true;
}

// Credentials ------------------------------------------------------------

bool LoadCredential(const std::string &server_id, Credential *out) {
  std::wstring target = kCredPrefix + Wide(server_id);
  PCREDENTIALW c = nullptr;
  if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &c)) return false;
  out->user = c->UserName ? Narrow(c->UserName, wcslen(c->UserName)) : "";
  out->password = Narrow(reinterpret_cast<const wchar_t *>(c->CredentialBlob),
                         c->CredentialBlobSize / sizeof(wchar_t));
  out->last_ip = c->Comment ? Narrow(c->Comment, wcslen(c->Comment)) : "";
  CredFree(c);
  return !out->user.empty() && !out->password.empty();
}

bool StoreCredential(const std::string &server_id, const Credential &cred, std::string *err) {
  std::wstring target = kCredPrefix + Wide(server_id);
  std::wstring user = Wide(cred.user);
  std::wstring password = Wide(cred.password);
  std::wstring comment = Wide(cred.last_ip);
  CREDENTIALW c{};
  c.Type = CRED_TYPE_GENERIC;
  c.TargetName = target.data();
  c.Comment = comment.data();
  c.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
  c.CredentialBlob = reinterpret_cast<LPBYTE>(password.data());
  c.Persist = CRED_PERSIST_LOCAL_MACHINE;
  c.UserName = user.data();
  if (!CredWriteW(&c, 0)) {
    *err = "CredWrite fehlgeschlagen, Fehler " + std::to_string(GetLastError());
    return false;
  }
  return true;
}

}  // namespace prospero
