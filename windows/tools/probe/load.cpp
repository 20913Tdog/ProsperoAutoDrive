// Prospero AutoDrive - prospero-probe: response time under load (P2.4)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// While one big file is written to the share and then read back, a small
// file is written and read every 2 s, and the server's queue counters are
// fetched over the service channel. The service channel is a separate TCP
// connection, so its response time tells apart a queue in the SMB
// connection (service fast, small file slow) from the network or a busy
// server (both slow).
//
// Big files go through without the client cache (FILE_FLAG_NO_BUFFERING),
// so every byte crosses the network, as with a big Explorer copy.
// Everything is created in a new test folder; nothing is deleted.

#include "load.h"

#include <winnetwk.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kChunk = 1024 * 1024;
constexpr DWORD kSmall = 4096;
constexpr int kSampleMs = 2000;

std::wstring Wide(const std::string &s) { return std::wstring(s.begin(), s.end()); }

int64_t NowMs() { return static_cast<int64_t>(GetTickCount64()); }

// Writes and reads back one small file; returns the time in ms, -1 on error.
int64_t SmallOp(const std::wstring &dir, int n) {
  std::wstring path = dir + L"\\small-" + std::to_wstring(n) + L".txt";
  std::string data(kSmall, static_cast<char>('a' + n % 26));
  int64_t t0 = NowMs();
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;
  DWORD done = 0;
  BOOL ok = WriteFile(h, data.data(), kSmall, &done, nullptr) && done == kSmall;
  CloseHandle(h);
  if (!ok) return -1;
  h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;
  std::string back(kSmall, '\0');
  ok = ReadFile(h, back.data(), kSmall, &done, nullptr) && done == kSmall && back == data;
  CloseHandle(h);
  return ok ? NowMs() - t0 : -1;
}

// First number of a "cur (max m, avg a)" status value.
long long Cur(const std::map<std::string, std::string> &st, const char *key) {
  auto it = st.find(key);
  return it == st.end() ? -1 : std::atoll(it->second.c_str());
}

std::string Max(const std::map<std::string, std::string> &st, const char *key) {
  auto it = st.find(key);
  if (it == st.end()) return "?";
  size_t p = it->second.find("(max ");
  return p == std::string::npos ? it->second : it->second.substr(p + 5, it->second.find(',', p) - p - 5);
}

double Mib(long long bytes) { return bytes / 1048576.0; }

int64_t Median(std::vector<int64_t> v) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

struct PhaseResult {
  bool ok = false;
  double mb_s = 0;
  std::vector<int64_t> small_ms;
  std::vector<int64_t> service_ms;
  std::map<std::string, std::string> last;
};

// Runs the big transfer in a thread and samples until it ends.
PhaseResult Phase(const char *title, bool write, const std::wstring &big, unsigned mib,
                  const std::wstring &dir, int *small_n, prospero::Channel &ch) {
  PhaseResult r;
  std::string err;
  if (!ch.ResetStats(&err)) {
    std::printf("  RESET-STATS: %s\n", err.c_str());
  }
  std::atomic<bool> running{true};
  std::atomic<bool> big_ok{false};
  std::atomic<long long> moved{0};
  int64_t t0 = NowMs();
  std::thread worker([&] {
    DWORD access = write ? GENERIC_WRITE : GENERIC_READ;
    DWORD disp = write ? CREATE_NEW : OPEN_EXISTING;
    HANDLE h = CreateFileW(big.c_str(), access, 0, nullptr, disp,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    void *buf = VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = h != INVALID_HANDLE_VALUE && buf;
    if (ok && write) {
      auto *p = static_cast<unsigned char *>(buf);
      for (DWORD i = 0; i < kChunk; i++) p[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    for (unsigned i = 0; ok && i < mib; i++) {
      DWORD done = 0;
      ok = write ? WriteFile(h, buf, kChunk, &done, nullptr) : ReadFile(h, buf, kChunk, &done, nullptr);
      ok = ok && done == kChunk;
      if (ok) moved += done;
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    big_ok = ok;
    running = false;
  });

  std::printf("  --- %s, %u MiB\n", title, mib);
  std::printf("  %6s %10s %10s %8s %12s %12s %7s %7s\n", "t [s]", "klein [ms]", "dienst [ms]",
              "outq", "outq [MiB]", "sock [MiB]", "waitq", "gap");
  while (running) {
    int64_t next = NowMs() + kSampleMs;
    while (running && NowMs() < next) Sleep(50);
    if (!running) break;
    int64_t small = SmallOp(dir, (*small_n)++);
    int64_t s0 = NowMs();
    std::map<std::string, std::string> st;
    bool got = ch.Status(&st, &err);
    int64_t service = NowMs() - s0;
    if (small >= 0) r.small_ms.push_back(small);
    if (got) {
      r.service_ms.push_back(service);
      r.last = st;
    }
    long long sock = Cur(st, "diag_sock_unsent_bytes");
    std::printf("  %6.0f %10lld %10lld %8lld %12.1f %12s %7lld %7lld\n", (NowMs() - t0) / 1000.0,
                static_cast<long long>(small), got ? static_cast<long long>(service) : -1LL,
                Cur(st, "diag_outq_pdus"), Mib(Cur(st, "diag_outq_bytes")),
                sock < 0 ? "?" : std::to_string(Mib(sock)).substr(0, 5).c_str(),
                Cur(st, "diag_waitq_pdus"), Cur(st, "diag_loop_gap_ms"));
  }
  worker.join();
  double secs = (NowMs() - t0) / 1000.0;
  r.ok = big_ok;
  r.mb_s = secs > 0 ? moved / 1e6 / secs : 0;
  std::map<std::string, std::string> st;
  if (ch.Status(&st, &err)) r.last = st;
  return r;
}

void Report(const char *title, const PhaseResult &r, int64_t idle, int *failures) {
  int64_t med = Median(r.small_ms);
  int64_t mx = r.small_ms.empty() ? -1 : *std::max_element(r.small_ms.begin(), r.small_ms.end());
  std::printf("  %s: %s, %.2f MB/s; kleine Datei Median %lld ms, max %lld ms (ohne Last %lld ms); "
              "Dienstkanal Median %lld ms\n",
              title, r.ok ? "OK" : "FEHLER", r.mb_s, static_cast<long long>(med),
              static_cast<long long>(mx), static_cast<long long>(idle),
              static_cast<long long>(Median(r.service_ms)));
  std::printf("  %s, Hoechstwerte im Server: outq %s Antworten, %s Bytes; sock %s Bytes; "
              "waitq %s; Pause %s ms; Sendepuffer %s Bytes\n",
              title, Max(r.last, "diag_outq_pdus").c_str(), Max(r.last, "diag_outq_bytes").c_str(),
              Max(r.last, "diag_sock_unsent_bytes").c_str(), Max(r.last, "diag_waitq_pdus").c_str(),
              Max(r.last, "diag_loop_gap_ms").c_str(),
              r.last.count("diag_sndbuf") ? r.last.at("diag_sndbuf").c_str() : "?");
  if (!r.ok) (*failures)++;
}

}  // namespace

int LoadTest(const std::string &ip, const std::string &unc_host, unsigned short port, const prospero::Hello &hello,
             const prospero::Credential &cred, const std::string &folder, std::string *test_dir,
             unsigned mib, int rounds) {
  int failures = 0;
  std::wstring share = L"\\\\" + Wide(unc_host) + L"\\" + Wide(hello.share);
  std::wstring user = Wide(cred.user);
  std::wstring password = Wide(cred.password);
  NETRESOURCEW nr{};
  nr.dwType = RESOURCETYPE_DISK;
  nr.lpRemoteName = share.data();
  DWORD rc = WNetAddConnection2W(&nr, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
  if (rc != NO_ERROR) {
    std::printf("FALSCH  Verbindung zur Freigabe, Windows-Fehler %lu\n", rc);
    return 1;
  }
  std::wstring dir = share + L"\\" + Wide(folder) + L"\\" + Wide(*test_dir);
  if (!CreateDirectoryW(dir.c_str(), nullptr)) {
    std::printf("FALSCH  Testordner anlegen, Windows-Fehler %lu\n", GetLastError());
    WNetCancelConnection2W(share.c_str(), 0, TRUE);
    return 1;
  }
  std::printf("OK      Testordner: %s\n", std::string(dir.begin(), dir.end()).c_str());

  prospero::Channel ch;
  prospero::Hello h2;
  std::string err;
  if (!ch.Connect(ip, port, 3000, &err) || !ch.SayHello(&h2, &err)) {
    std::printf("FALSCH  Dienstkanal: %s\n", err.c_str());
    WNetCancelConnection2W(share.c_str(), 0, TRUE);
    return 1;
  }

  int small_n = 0;
  std::vector<int64_t> idle_v;
  for (int i = 0; i < 5; i++) {
    int64_t t = SmallOp(dir, small_n++);
    if (t >= 0) idle_v.push_back(t);
  }
  int64_t idle = Median(idle_v);
  std::printf("OK      kleine Datei ohne Last: Median %lld ms aus %zu Versuchen\n",
              static_cast<long long>(idle), idle_v.size());

  for (int r = 1; r <= rounds; r++) {
    std::printf("\n  ===== Runde %d von %d =====\n", r, rounds);
    std::wstring big = dir + L"\\big-" + std::to_wstring(r) + L".bin";
    PhaseResult w = Phase("Schreiben", true, big, mib, dir, &small_n, ch);
    PhaseResult rd = Phase("Lesen", false, big, mib, dir, &small_n, ch);
    std::printf("\n");
    Report("Schreiben", w, idle, &failures);
    Report("Lesen", rd, idle, &failures);
  }
  ch.Close();
  WNetCancelConnection2W(share.c_str(), 0, TRUE);
  return failures;
}
