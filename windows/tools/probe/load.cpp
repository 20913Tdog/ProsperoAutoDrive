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
// Big files are copied with CopyFileEx without buffering, as the Explorer
// does, so every byte crosses the network with several requests in flight.
// The source for writing is a local file in %TEMP%, deleted at the end, as
// is the local copy of each read. On the share everything is created in a
// new test folder; nothing there is deleted.

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
// A copy that moves no byte for this long counts as hung: it is cancelled
// and the load test ends, instead of waiting forever.
constexpr int64_t kStallMs = 60000;

std::wstring Wide(const std::string &s) { return std::wstring(s.begin(), s.end()); }

int64_t NowMs() { return static_cast<int64_t>(GetTickCount64()); }

// Progress bar on the console window itself (CONOUT$), so it shows while
// Run-Test.cmd collects stdout in a log, and never ends up in that log.
// Without a console (CI) it does nothing.
class Bar {
 public:
  Bar() {
    h_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     nullptr, OPEN_EXISTING, 0, nullptr);
  }
  ~Bar() {
    Clear();
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }
  void Show(const std::string &line) {
    if (h_ == INVALID_HANDLE_VALUE) return;
    std::string out = "\r" + line;
    if (line.size() < shown_) out += std::string(shown_ - line.size(), ' ');
    Write(out);
    shown_ = line.size();
  }
  // Before other output to the same console.
  void Clear() {
    if (h_ == INVALID_HANDLE_VALUE || !shown_) return;
    Write("\r" + std::string(shown_, ' ') + "\r");
    shown_ = 0;
  }

 private:
  void Write(const std::string &s) {
    DWORD n = 0;
    WriteConsoleA(h_, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
  }
  HANDLE h_ = INVALID_HANDLE_VALUE;
  size_t shown_ = 0;
};

Bar g_bar;
std::string g_bar_label;      // e.g. "Runde 2/3"

void ShowProgress(const char *title, long long done, long long want, int64_t elapsed_ms) {
  constexpr int kWidth = 30;
  int pct = want > 0 ? static_cast<int>(done * 100 / want) : 0;
  int fill = want > 0 ? static_cast<int>(done * kWidth / want) : 0;
  double mb_s = elapsed_ms > 0 ? done / 1e3 / elapsed_ms : 0;
  long long left = mb_s > 0 ? static_cast<long long>((want - done) / 1e6 / mb_s) : -1;
  char buf[200];
  std::snprintf(buf, sizeof buf, "  %s %-9s [%s%s] %3d %%  %5lld/%lld MiB  %6.1f MB/s  %s",
                g_bar_label.c_str(), title, std::string(fill, '#').c_str(),
                std::string(kWidth - fill, '-').c_str(), pct, done >> 20, want >> 20, mb_s,
                left >= 0 ? ("noch " + std::to_string(left) + " s").c_str() : "");
  g_bar.Show(buf);
}

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
  bool hung = false;
  double mb_s = 0;
  double secs = 0;
  std::vector<int64_t> small_ms;
  std::vector<int64_t> service_ms;
  std::map<std::string, std::string> last;
};

// Runs the big transfer in a thread and samples until it ends.
DWORD CALLBACK Progress(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD,
                        DWORD, HANDLE, HANDLE, LPVOID data) {
  static_cast<std::atomic<long long> *>(data)->store(done.QuadPart);
  return PROGRESS_CONTINUE;
}

// A local file of mib MiB in %TEMP%, the source for writing to the share.
bool MakeLocal(const std::wstring &path, unsigned mib) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  std::vector<unsigned char> buf(kChunk);
  bool ok = true;
  for (unsigned i = 0; ok && i < mib; i++) {
    for (DWORD j = 0; j < kChunk; j++) buf[j] = static_cast<unsigned char>(j * 7 + i * 13 + 3);
    DWORD done = 0;
    ok = WriteFile(h, buf.data(), kChunk, &done, nullptr) && done == kChunk;
  }
  CloseHandle(h);
  return ok;
}

PhaseResult Phase(const char *title, bool write, const std::wstring &big, const std::wstring &local,
                  unsigned mib, const std::wstring &dir, int *small_n, prospero::Channel &ch,
                  const std::string &ch_ip, unsigned short ch_port) {
  PhaseResult r;
  std::string err;
  if (!ch.ResetStats(&err)) {
    std::printf("  RESET-STATS: %s\n", err.c_str());
  }
  struct Job {
    std::wstring from, to;
    bool write;
    long long want;
    volatile BOOL cancel = FALSE;
    std::atomic<bool> running{true};
    std::atomic<bool> ok{false};
    std::atomic<long long> moved{0};
  };
  // Shared with the thread; leaked on purpose if the thread never returns.
  Job *job = new Job;
  job->from = write ? local : big;
  job->to = write ? big : local + L".back";
  job->write = write;
  job->want = static_cast<long long>(mib) * kChunk;
  int64_t t0 = NowMs();
  // CopyFileEx without buffering, as the Explorer copies big files: it
  // keeps several requests in flight, unlike a loop of ReadFile calls.
  HANDLE worker = CreateThread(nullptr, 0, [](LPVOID arg) -> DWORD {
    Job *j = static_cast<Job *>(arg);
    bool ok = CopyFileExW(j->from.c_str(), j->to.c_str(), Progress, &j->moved,
                          const_cast<BOOL *>(&j->cancel),
                          COPY_FILE_NO_BUFFERING | COPY_FILE_FAIL_IF_EXISTS) != 0;
    if (!ok) std::printf("  CopyFileEx: Windows-Fehler %lu\n", GetLastError());
    if (!j->write) DeleteFileW(j->to.c_str());    // local copy on the PC only
    j->ok = ok && j->moved == j->want;
    j->running = false;
    return 0;
  }, job, 0, nullptr);
  std::atomic<bool> &running = job->running;
  long long last_moved = 0;
  int64_t last_progress = t0;

  std::printf("  --- %s, %u MiB\n", title, mib);
  std::printf("  %6s %10s %10s %8s %12s %12s %7s %7s\n", "t [s]", "klein [ms]", "dienst [ms]",
              "outq", "outq [MiB]", "sock [MiB]", "waitq", "gap");
  while (running) {
    int64_t next = NowMs() + kSampleMs;
    while (running && NowMs() < next) {
      ShowProgress(title, job->moved, job->want, NowMs() - t0);
      Sleep(250);
    }
    if (!running) break;
    if (job->moved != last_moved) {
      last_moved = job->moved;
      last_progress = NowMs();
    } else if (NowMs() - last_progress > kStallMs) {
      std::printf("  HAENGT: seit %lld s kein Byte bewegt (%.0f von %u MiB), Kopie wird abgebrochen\n",
                  static_cast<long long>(kStallMs / 1000), Mib(last_moved), mib);
      r.hung = true;
      job->cancel = TRUE;
      CancelSynchronousIo(worker);
      break;
    }
    int64_t small = SmallOp(dir, (*small_n)++);
    int64_t s0 = NowMs();
    std::map<std::string, std::string> st;
    bool got = ch.Status(&st, &err);
    if (!got) {
      // The channel times out after 5 s; a new one keeps the samples coming.
      std::string e2;
      prospero::Hello h;
      got = ch.Connect(ch_ip, ch_port, 3000, &e2) && ch.SayHello(&h, &e2) && ch.Status(&st, &e2);
    }
    int64_t service = NowMs() - s0;
    if (small >= 0) r.small_ms.push_back(small);
    if (got) {
      r.service_ms.push_back(service);
      r.last = st;
    }
    long long sock = Cur(st, "diag_sock_unsent_bytes");
    g_bar.Clear();
    std::printf("  %6.0f %10lld %10lld %8lld %12.1f %12s %7lld %7lld\n", (NowMs() - t0) / 1000.0,
                static_cast<long long>(small), got ? static_cast<long long>(service) : -1LL,
                Cur(st, "diag_outq_pdus"), Mib(Cur(st, "diag_outq_bytes")),
                sock < 0 ? "?" : std::to_string(Mib(sock)).substr(0, 5).c_str(),
                Cur(st, "diag_waitq_pdus"), Cur(st, "diag_loop_gap_ms"));
  }
  bool done = WaitForSingleObject(worker, r.hung ? 30000 : INFINITE) == WAIT_OBJECT_0;
  g_bar.Clear();
  CloseHandle(worker);
  double secs = (NowMs() - t0) / 1000.0;
  long long moved = job->moved;
  r.ok = done && !r.hung && job->ok;
  r.secs = secs;
  r.mb_s = secs > 0 ? moved / 1e6 / secs : 0;
  if (done) {
    delete job;
  } else {
    std::printf("  Die Kopie reagiert nicht auf den Abbruch, sie bleibt im Hintergrund.\n");
  }
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
  // Where the server spent the phase: computing (CPU) or in the disk calls.
  auto num = [&](const char *k) {
    return r.last.count(k) ? std::atoll(r.last.at(k).c_str()) : -1LL;
  };
  long long span = num("since_reset_ms");
  long long cpu = num("cpu_user_ms") + num("cpu_sys_ms");
  if (span > 0) {
    std::printf("  %s, Server: Rechenzeit %lld ms (davon System %lld) in %lld ms, also %lld %%; "
                "pread %lld ms, pwrite %lld ms (%lld von %lld im Schreib-Thread, "
                "darauf gewartet %lld ms)\n",
                title, cpu, num("cpu_sys_ms"), span, cpu * 100 / span, num("pread_ms"),
                num("pwrite_ms"), num("writes_async"), num("writes"), num("write_wait_ms"));
  }
  if (!r.ok) (*failures)++;
}

}  // namespace



namespace {

struct Variant {
  int sndbuf = 0;      // bytes, 0 = server default
  int credits = 0;     // 0 = no limit
  std::string Name() const {
    std::string s = sndbuf ? std::to_string(sndbuf / 1024) + " KiB" : "Standard";
    return "Puffer " + s + ", Kredite " + (credits ? std::to_string(credits) : "frei");
  }
  std::vector<PhaseResult> writes, reads;
};

// "sndbuf:credits,..." -> variants; empty or invalid -> one default variant.
std::vector<Variant> ParseVariants(const std::string &spec) {
  std::vector<Variant> out;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    size_t colon = item.find(':');
    if (colon != std::string::npos) {
      Variant v;
      v.sndbuf = std::atoi(item.substr(0, colon).c_str());
      v.credits = std::atoi(item.substr(colon + 1).c_str());
      out.push_back(v);
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  if (out.empty()) out.emplace_back();
  return out;
}

int64_t MedianOf(const std::vector<PhaseResult> &v, bool small) {
  std::vector<int64_t> all;
  for (const auto &r : v) {
    if (small) {
      all.insert(all.end(), r.small_ms.begin(), r.small_ms.end());
    } else {
      all.push_back(static_cast<int64_t>(r.mb_s * 100));
    }
  }
  return Median(all);
}

}  // namespace

int LoadTest(const std::string &ip, const std::string &unc_host, unsigned short port,
             const prospero::Hello &hello, const prospero::Credential &cred,
             const std::string &folder, std::string *test_dir, unsigned mib, int rounds,
             const std::string &variants, const std::string &round_cmd) {
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

  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring local = std::wstring(tmp) + L"prospero-probe-" +
                       Wide(prospero::RandomHex(4)) + L".bin";
  if (!MakeLocal(local, mib)) {
    std::printf("FALSCH  lokale Quelldatei in %%TEMP%% anlegen, Windows-Fehler %lu\n", GetLastError());
    ch.Close();
    WNetCancelConnection2W(share.c_str(), 0, TRUE);
    return 1;
  }

  std::vector<Variant> vs = ParseVariants(variants);
  bool tuning = !variants.empty();
  bool hung = false;
  for (int r = 1; r <= rounds; r++) {
    // Every other round runs the variants backwards, so that a slow drift of
    // the network does not favour one of them.
    for (size_t k = 0; k < vs.size(); k++) {
      size_t i = (r % 2) ? k : vs.size() - 1 - k;
      Variant &v = vs[i];
      std::printf("\n  ===== Runde %d von %d, Variante %zu: %s =====\n", r, rounds, i + 1,
                  v.Name().c_str());
      g_bar_label = "Runde " + std::to_string(r) + "/" + std::to_string(rounds);
      if (vs.size() > 1) g_bar_label += " V" + std::to_string(i + 1);
      if (tuning && !ch.Tune(cred.password, v.sndbuf, v.credits, &err)) {
        std::printf("FALSCH  TUNE: %s\n", err.c_str());
        failures++;
        continue;
      }
      std::wstring big = dir + L"\\big-" + std::to_wstring(r) + L"-" + std::to_wstring(i + 1) + L".bin";
      PhaseResult w = Phase("Schreiben", true, big, local, mib, dir, &small_n, ch, ip, port);
      PhaseResult rd;
      if (!w.hung) rd = Phase("Lesen", false, big, local, mib, dir, &small_n, ch, ip, port);
      std::printf("\n");
      Report("Schreiben", w, idle, &failures);
      if (!w.hung) Report("Lesen", rd, idle, &failures);
      v.writes.push_back(w);
      v.reads.push_back(rd);
      if (w.hung || rd.hung) {
        std::printf("FALSCH  Eine Kopie hing, der Lasttest endet hier.\n");
        hung = true;
        break;
      }
    }
    if (hung) break;
    if (!round_cmd.empty()) {
      // cmd /c strips the outer quotes and keeps those around the path. The
      // script inherits stdout, so its lines land in the same log.
      std::string cmd = "\"\"" + round_cmd + "\" --round " + std::to_string(r) + "\"";
      int rc = std::system(cmd.c_str());
      if (rc != 0) std::printf("  Rundenbefehl: Exitcode %d\n", rc);
    }
  }
  if (tuning && !ch.Tune(cred.password, 0, 0, &err)) {
    std::printf("FALSCH  TUNE zurueck auf Standard: %s\n", err.c_str());
    failures++;
  }

  std::printf("\n  ===== Zusammenfassung (Median ueber alle Runden), ohne Last %lld ms =====\n",
              static_cast<long long>(idle));
  std::printf("  %-32s %12s %14s %12s %14s\n", "Variante", "Schr. MB/s", "Schr. klein ms",
              "Lesen MB/s", "Lesen klein ms");
  for (size_t i = 0; i < vs.size(); i++) {
    const Variant &v = vs[i];
    std::printf("  %zu %-30s %12.2f %14lld %12.2f %14lld\n", i + 1, v.Name().c_str(),
                MedianOf(v.writes, false) / 100.0, static_cast<long long>(MedianOf(v.writes, true)),
                MedianOf(v.reads, false) / 100.0, static_cast<long long>(MedianOf(v.reads, true)));
  }
  DeleteFileW(local.c_str());                        // on the PC only
  ch.Close();
  WNetCancelConnection2W(share.c_str(), 0, TRUE);
  return failures;
}

namespace {

int64_t ProcessCpuMs() {
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  auto ms = [](const FILETIME &f) {
    return static_cast<int64_t>((static_cast<uint64_t>(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 10000);
  };
  return ms(k) + ms(u);
}

// One idle phase: waits `seconds`, with a PING every second if ping is set,
// then compares the CPU time of the server and of this process with the
// elapsed time. Returns false if the 0.1 % goal is missed or the channel
// failed.
bool IdlePhase(const char *title, bool ping, int seconds, prospero::Channel &ch,
               const std::string &key) {
  std::string err;
  if (!ch.ResetStats(&err)) {
    std::printf("FALSCH  %s: RESET-STATS: %s\n", title, err.c_str());
    return false;
  }
  int64_t t0 = NowMs();
  int64_t cpu0 = ProcessCpuMs();
  int pings = 0, lost = 0;
  for (int s = 1; s <= seconds; s++) {
    // The next full second after the start, so a slow PING does not add up.
    int64_t wait = t0 + s * 1000LL - NowMs();
    if (wait > 0) Sleep(static_cast<DWORD>(wait));
    if (ping) {
      prospero::Pong p;
      if (ch.Ping(key, &p, &err)) {
        pings++;
      } else {
        lost++;
      }
    }
    char line[120];
    std::snprintf(line, sizeof line, "  %s: %d von %d s", title, s, seconds);
    g_bar.Show(line);
  }
  int64_t wall = NowMs() - t0;
  int64_t pc_cpu = ProcessCpuMs() - cpu0;
  g_bar.Clear();
  std::map<std::string, std::string> st;
  if (!ch.Status(&st, &err)) {
    std::printf("FALSCH  %s: STATUS: %s\n", title, err.c_str());
    return false;
  }
  auto num = [&](const char *k) { return st.count(k) ? std::atoll(st.at(k).c_str()) : -1LL; };
  long long span = num("since_reset_ms");
  long long srv_cpu = num("cpu_user_ms") + num("cpu_sys_ms");
  double srv_pct = span > 0 ? srv_cpu * 100.0 / span : -1;
  double pc_pct = wall > 0 ? pc_cpu * 100.0 / wall : -1;
  bool ok = srv_pct >= 0 && srv_pct < 0.1 && pc_pct >= 0 && pc_pct < 0.1 && lost == 0;
  std::printf("%s %s: %lld s, PING %d beantwortet, %d ohne Antwort\n", ok ? "OK     " : "FALSCH ",
              title, static_cast<long long>(wall / 1000), pings, lost);
  std::printf("          Server: %lld ms Rechenzeit (System %lld) in %lld ms, also %.3f %% eines Kerns\n",
              srv_cpu, num("cpu_sys_ms"), span, srv_pct);
  std::printf("          PC (dieses Programm): %lld ms in %lld ms, also %.3f %% eines Kerns\n",
              static_cast<long long>(pc_cpu), static_cast<long long>(wall), pc_pct);
  std::printf("          Ziel: unter 0,1 %% auf beiden Seiten\n");
  return ok;
}

}  // namespace

int IdleTest(const std::string &ip, const std::string &unc_host, unsigned short port,
             const prospero::Hello &hello, const prospero::Credential &cred, int seconds) {
  int failures = 0;
  // The share stays connected like P: in normal use; nothing is read.
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
  std::printf("OK      Freigabe verbunden, es wird nichts gelesen oder geschrieben\n");
  prospero::Channel ch;
  prospero::Hello h2;
  std::string err;
  if (!ch.Connect(ip, port, 3000, &err) || !ch.SayHello(&h2, &err)) {
    std::printf("FALSCH  Dienstkanal: %s\n", err.c_str());
    WNetCancelConnection2W(share.c_str(), 0, TRUE);
    return 1;
  }
  // The server closes a silent channel after 60 s, so the quiet phase is
  // shorter than that.
  int quiet = seconds / 2 < 30 ? seconds / 2 : 30;
  if (quiet > 0 && !IdlePhase("ohne Lebenszeichen", false, quiet, ch, cred.password)) failures++;
  if (!IdlePhase("Lebenszeichen jede Sekunde", true, seconds, ch, cred.password)) failures++;
  ch.Close();
  WNetCancelConnection2W(share.c_str(), 0, TRUE);
  return failures;
}
