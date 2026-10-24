// Prospero AutoDrive - prospero-probe: diagnosis of a server, step by step
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT
//
// Does what ProsperoAutoDrive.exe will do, one step after the other, and
// prints every result:
//   1. find the server by UDP: unicast to the address, then broadcast
//   2. service channel on TCP: HELLO
//   3. pairing: use the stored credential, or pair if the server allows it
//      and store user and password in the Windows Credential Manager
//   4. PING with and without mac (liveness, clock skew)
//   5. optional: OPEN-PAIRING, to let another PC pair for a while
//   6. optional: connect to the share and write into a new test folder
//   7. optional: keep a connection to the share for this logon, so the
//      Explorer can open it without asking (until ProsperoAutoDrive.exe
//      does that in phase 3)
//
//   prospero-probe.exe <IPv4 address> [--port N] [--no-broadcast]
//                      [--open-pairing] [--write-test <folder in the share>]
//                      [--connect] [--load-test <folder> <MiB> [--rounds N]]
//                      [--share-host <name>] [--variants <sndbuf:credits,...>]
//                      [--test-name <_ProsperoAutoDrive_Test_...>]
//                      [--round-cmd <file.cmd>] [--idle-test <seconds>]
//
// --share-host names the server in \\<name>\<share> when that is not the
// address itself (CI: a hosts alias with an alternate SMB port).
//
// --load-test measures the response time under load (load.cpp), in its own
// new test folder below <folder>. --round-cmd runs a script after each
// round with "--round <n>" (P24: ftpsrv as the reference, alternately).
//
// --idle-test measures the CPU time of server and PC while the share is
// connected and idle, with and without a PING every second (load.cpp).
//
// --write-test creates <folder>\_ProsperoAutoDrive_Test_<UTC time>-<random>
// in the share, writes a file into it and reads it back. Nothing that
// exists already is changed, and nothing is deleted.
//
// Exit code 0 if every step passed, 1 otherwise, 2 on wrong arguments.

#include "client.h"
#include "load.h"

#include <winnetwk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Ok(const std::string &what) { std::printf("OK      %s\n", what.c_str()); }

void Fail(const std::string &what) {
  std::printf("FALSCH  %s\n", what.c_str());
  g_failures++;
}

std::wstring Wide(const std::string &s) {
  return std::wstring(s.begin(), s.end());   // ASCII only: paths built here
}

std::string TestFolderName() {
  SYSTEMTIME t;
  GetSystemTime(&t);
  char buf[64];
  std::snprintf(buf, sizeof buf, "_ProsperoAutoDrive_Test_%04u%02u%02u-%02u%02u%02u-%s",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                prospero::RandomHex(3).c_str());
  return buf;
}

// Step 6. Returns true if the file came back unchanged.
bool WriteTest(const std::string &ip, const prospero::Hello &hello,
               const prospero::Credential &cred, const std::string &folder) {
  std::wstring share = L"\\\\" + Wide(ip) + L"\\" + Wide(hello.share);
  std::wstring user = Wide(cred.user);
  std::wstring password(cred.password.begin(), cred.password.end());
  NETRESOURCEW nr{};
  nr.dwType = RESOURCETYPE_DISK;
  nr.lpRemoteName = share.data();
  DWORD rc = WNetAddConnection2W(&nr, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
  if (rc != NO_ERROR) {
    Fail("Verbindung zur Freigabe " + std::string(share.begin(), share.end()) +
         ", Windows-Fehler " + std::to_string(rc));
    return false;
  }
  Ok("Verbindung zur Freigabe " + std::string(share.begin(), share.end()));

  bool good = false;
  std::string name = TestFolderName();
  std::wstring dir = share + L"\\" + Wide(folder) + L"\\" + Wide(name);
  std::string dir_text(dir.begin(), dir.end());
  if (!CreateDirectoryW(dir.c_str(), nullptr)) {
    Fail("Testordner anlegen " + dir_text + ", Windows-Fehler " + std::to_string(GetLastError()));
  } else {
    Ok("Testordner angelegt: " + dir_text);
    // What the Explorer checks before it copies into this folder.
    ULARGE_INTEGER free_bytes{}, total_bytes{};
    if (GetDiskFreeSpaceExW((dir + L"\\").c_str(), &free_bytes, &total_bytes, nullptr)) {
      std::printf("  freier Speicher laut Windows: %.1f GB von %.1f GB\n",
                  free_bytes.QuadPart / 1e9, total_bytes.QuadPart / 1e9);
    } else {
      Fail("freien Speicher abfragen, Windows-Fehler " + std::to_string(GetLastError()));
    }
    std::wstring file = dir + L"\\hello.txt";
    std::string text = "Prospero AutoDrive probe " + prospero::RandomHex(8) + "\r\n";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD n = 0;
    bool written = h != INVALID_HANDLE_VALUE &&
                   WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr) &&
                   n == text.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!written) {
      Fail("Datei schreiben, Windows-Fehler " + std::to_string(GetLastError()));
    } else {
      char back[256] = {};
      h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
      n = 0;
      if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, back, sizeof back - 1, &n, nullptr);
        CloseHandle(h);
      }
      if (std::string(back, n) == text) {
        Ok("Datei geschrieben und gleich zurueckgelesen (" + std::to_string(n) + " Bytes)");
        good = true;
        // The server stamps a new file with its own clock and reports it
        // corrected by the measured skew, so it should match this PC.
        WIN32_FILE_ATTRIBUTE_DATA fa;
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa)) {
          auto ticks = [](const FILETIME &f) {
            return static_cast<int64_t>((static_cast<uint64_t>(f.dwHighDateTime) << 32) |
                                        f.dwLowDateTime);
          };
          int64_t diff_s = (ticks(fa.ftLastWriteTime) - ticks(now)) / 10000000;
          std::string what = "Aenderungszeit der neuen Datei weicht " + std::to_string(diff_s) +
                             " s von der PC-Uhr ab";
          if (diff_s >= -10 && diff_s <= 10) {
            Ok(what);
          } else {
            Fail(what + " (erlaubt: 10 s)");
            good = false;
          }
        } else {
          Fail("Aenderungszeit lesen, Windows-Fehler " + std::to_string(GetLastError()));
          good = false;
        }
      } else {
        Fail("Datei zurueckgelesen, aber anders (" + std::to_string(n) + " Bytes)");
      }
    }
  }
  WNetCancelConnection2W(share.c_str(), 0, TRUE);
  return good;
}

}  // namespace

int main(int argc, char **argv) {
  std::string ip;
  unsigned short port = prospero::kServicePort;
  bool broadcast = true;
  bool open_pairing = false;
  std::string write_folder;
  bool connect = false;
  std::string load_folder;
  unsigned load_mib = 0;
  int rounds = 1;
  std::string share_host;      // name in \\host\share; default: the address
  std::string variants;        // --variants for the load test
  std::string test_name;       // --test-name: folder name for the load test
  std::string round_cmd;       // --round-cmd: script after each load round
  int idle_seconds = 0;        // --idle-test
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--connect")) {
      connect = true;
    } else if (!std::strcmp(argv[i], "--load-test") && i + 2 < argc) {
      load_folder = argv[++i];
      load_mib = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (!std::strcmp(argv[i], "--variants") && i + 1 < argc) {
      variants = argv[++i];
    } else if (!std::strcmp(argv[i], "--test-name") && i + 1 < argc) {
      test_name = argv[++i];
    } else if (!std::strcmp(argv[i], "--idle-test") && i + 1 < argc) {
      idle_seconds = std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--round-cmd") && i + 1 < argc) {
      round_cmd = argv[++i];
    } else if (!std::strcmp(argv[i], "--share-host") && i + 1 < argc) {
      share_host = argv[++i];
    } else if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc) {
      rounds = std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--no-broadcast")) {
      broadcast = false;
    } else if (!std::strcmp(argv[i], "--open-pairing")) {
      open_pairing = true;
    } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
      port = static_cast<unsigned short>(std::atoi(argv[++i]));
    } else if (!std::strcmp(argv[i], "--write-test") && i + 1 < argc) {
      write_folder = argv[++i];
    } else if (argv[i][0] != '-' && ip.empty()) {
      ip = argv[i];
    } else {
      ip.clear();
      break;
    }
  }
  if (ip.empty()) {
    std::printf("Aufruf: prospero-probe.exe <IPv4-Adresse> [--port N] [--no-broadcast]\n"
                "                           [--open-pairing] [--write-test <Ordner>]\n"
                "                           [--connect]\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  const std::string unc = share_host.empty() ? ip : share_host;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::printf("FEHLER: WSAStartup\n");
    return 1;
  }
  std::string err;
  std::printf("PC %s, Server %s, Port %u\n", prospero::PcName().c_str(), ip.c_str(), port);

  std::printf("\n===== 1. Suche per UDP =====\n");
  in_addr addr{};
  inet_pton(AF_INET, ip.c_str(), &addr);
  std::vector<prospero::Here> found;
  std::string disc_id;
  if (!prospero::Discover({addr}, port, 1500, &found, &err)) {
    Fail("Suche: " + err);
  } else if (found.empty()) {
    Fail("Unicast an " + ip + ": keine Antwort");
  } else {
    disc_id = found[0].id;
    Ok("Unicast an " + ip + ": Antwort nach " + std::to_string(found[0].rtt_ms) + " ms: " +
       found[0].line);
  }
  if (broadcast) {
    auto targets = prospero::BroadcastTargets();
    std::vector<in_addr> addrs;
    for (const auto &t : targets) {
      std::printf("  Netz: %s, Broadcast %s\n", t.second.c_str(), prospero::Ip(t.first).c_str());
      addrs.push_back(t.first);
    }
    found.clear();
    if (!prospero::Discover(addrs, port, 1500, &found, &err)) {
      Fail("Broadcast: " + err);
    } else {
      bool seen = false;
      for (const auto &h : found) {
        std::printf("  Antwort von %s nach %lld ms, id=%s\n", h.ip.c_str(),
                    static_cast<long long>(h.rtt_ms), h.id.c_str());
        seen |= h.ip == ip;
      }
      if (seen) {
        Ok("Broadcast: " + ip + " hat geantwortet");
      } else {
        Fail("Broadcast: keine Antwort von " + ip);
      }
    }
  }

  std::printf("\n===== 2. Dienstkanal per TCP =====\n");
  prospero::Channel ch;
  prospero::Hello hello;
  if (!ch.Connect(ip, port, 3000, &err) || !ch.SayHello(&hello, &err)) {
    Fail("HELLO: " + err);
    std::printf("\nPROBE: NICHT BESTANDEN, %d Fehler\n", g_failures);
    return 1;
  }
  {
    // Without the nonce, which only keys this connection.
    std::string shown = hello.line;
    size_t p = shown.find(" nonce=");
    if (p != std::string::npos) shown.erase(p, shown.find(' ', p + 1) - p);
    Ok(shown);
  }
  if (!disc_id.empty() && disc_id != hello.server_id) {
    Fail("Server-ID der Suche (" + disc_id + ") und des Dienstkanals verschieden");
  }

  std::printf("\n===== 3. Kopplung =====\n");
  prospero::Credential cred;
  bool have = prospero::LoadCredential(hello.server_id, &cred);
  if (have) {
    Ok("bereits gekoppelt, Zugangsdaten aus der Anmeldeinformationsverwaltung (Benutzer " +
       cred.user + ")");
    if (cred.last_ip != ip) {
      cred.last_ip = ip;
      prospero::StoreCredential(hello.server_id, cred, &err);
    }
  } else if (!hello.pairing_open) {
    Fail("nicht gekoppelt, und der Server laesst gerade kein Koppeln zu");
  } else {
    std::string refused;
    if (ch.Pair(prospero::PcName(), &cred.user, &cred.password, &refused, &err)) {
      cred.last_ip = ip;
      if (prospero::StoreCredential(hello.server_id, cred, &err)) {
        Ok("gekoppelt als " + prospero::PcName() + ", Zugangsdaten gespeichert (Benutzer " +
           cred.user + ")");
        have = true;
      } else {
        Fail("gekoppelt, aber nicht gespeichert: " + err);
      }
    } else {
      Fail("Koppeln abgelehnt: " + (refused.empty() ? err : refused));
    }
  }

  std::printf("\n===== 4. Lebenszeichen =====\n");
  if (have) {
    for (int i = 1; i <= 3; i++) {
      prospero::Pong p;
      if (!ch.Ping(cred.password, &p, &err)) {
        Fail("PING " + std::to_string(i) + ": " + err);
        break;
      }
      std::printf("  PING %d: Antwort nach %lld ms, Abweichung Server - PC %lld ms, trusted=%d\n",
                  i, static_cast<long long>(p.rtt_ms), static_cast<long long>(p.skew_ms),
                  p.trusted ? 1 : 0);
      if (!p.trusted) Fail("PING mit Pruefsumme nicht anerkannt");
      if (i < 3) Sleep(1000);
    }
  }
  prospero::Pong plain;
  if (!ch.Ping("", &plain, &err)) {
    Fail("PING ohne Pruefsumme: " + err);
  } else if (plain.trusted) {
    Fail("PING ohne Pruefsumme wurde anerkannt");
  } else {
    Ok("PING ohne Pruefsumme beantwortet, aber nicht fuer die Uhr verwendet");
  }

  if (open_pairing) {
    std::printf("\n===== 5. Weiteren PC koppeln =====\n");
    int seconds = 0;
    if (!have) {
      Fail("nicht gekoppelt, kann kein Koppeln oeffnen");
    } else if (ch.OpenPairing(cred.password, &seconds, &err)) {
      Ok("Koppeln fuer " + std::to_string(seconds / 60) + " Minuten offen");
    } else {
      Fail("OPEN-PAIRING: " + err);
    }
  }
  ch.Close();

  if (!write_folder.empty()) {
    std::printf("\n===== 6. Freigabe: Testordner schreiben =====\n");
    if (have) {
      WriteTest(unc, hello, cred, write_folder);
    } else {
      Fail("ohne Zugangsdaten kein Schreibtest");
    }
  }

  if (!load_folder.empty()) {
    std::printf("\n===== 8. Antwortzeit unter Last =====\n");
    if (!have) {
      Fail("ohne Zugangsdaten kein Lasttest");
    } else if (load_mib < 1 || rounds < 1) {
      Fail("--load-test braucht eine Groesse ab 1 MiB und --rounds ab 1");
    } else {
      std::string name = TestFolderName();
      if (!test_name.empty()) name = test_name;
      int f = LoadTest(ip, unc, port, hello, cred, load_folder, &name, load_mib, rounds, variants,
                       round_cmd);
      if (f) {
        Fail("Lasttest: " + std::to_string(f) + " Uebertragung(en) fehlgeschlagen");
      } else {
        Ok("Lasttest durchgelaufen");
      }
    }
  }

  if (idle_seconds > 0) {
    std::printf("\n===== 9. Leerlauf: Rechenzeit =====\n");
    if (!have) {
      Fail("ohne Zugangsdaten kein Leerlauftest");
    } else {
      int f = IdleTest(ip, unc, port, hello, cred, idle_seconds);
      if (f) {
        Fail("Leerlauf: " + std::to_string(f) + " Phase(n) ueber 0,1 % oder ohne Antwort");
      } else {
        Ok("Leerlauf unter 0,1 % auf Server und PC");
      }
    }
  }

  if (connect) {
    std::printf("\n===== 7. Freigabe fuer den Explorer verbinden =====\n");
    if (!have) {
      Fail("ohne Zugangsdaten keine Verbindung");
    } else {
      // Temporary: it ends with the logon, nothing is remembered.
      std::wstring share = L"\\\\" + Wide(unc) + L"\\" + Wide(hello.share);
      std::wstring user = Wide(cred.user);
      std::wstring password = Wide(cred.password);
      NETRESOURCEW nr{};
      nr.dwType = RESOURCETYPE_DISK;
      nr.lpRemoteName = share.data();
      DWORD rc = WNetAddConnection2W(&nr, password.c_str(), user.c_str(), CONNECT_TEMPORARY);
      std::string text(share.begin(), share.end());
      if (rc == NO_ERROR) {
        Ok(text + " ist bis zur Abmeldung verbunden, der Explorer fragt nicht mehr nach dem Passwort");
      } else if (rc == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        Fail(text + ": es besteht schon eine Verbindung mit anderen Anmeldedaten (Fehler 1219). "
             "Explorer-Fenster schliessen, \"net use " + text + " /delete\" ausfuehren und neu starten");
      } else {
        Fail(text + ", Windows-Fehler " + std::to_string(rc));
      }
    }
  }

  WSACleanup();
  std::printf("\n");
  if (g_failures) {
    std::printf("PROBE: NICHT BESTANDEN, %d Fehler\n", g_failures);
    return 1;
  }
  std::printf("PROBE: BESTANDEN\n");
  return 0;
}
