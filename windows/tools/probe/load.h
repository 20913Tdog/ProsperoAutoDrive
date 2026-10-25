// Prospero AutoDrive - prospero-probe: response time under load (P2.4)
//
// Copyright (C) 2026 Prospero AutoDrive contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "client.h"

#include <string>

// Creates <folder>\<test_dir> in the share, then per round writes and reads
// a big file of mib MiB while sampling small-file time and server queues.
// Prints a table per phase and a summary. variants is "sndbuf:credits,..."
// set on the server with TUNE before each (empty: no TUNE); every round
// runs all of them, every other round backwards. After each round,
// round_cmd (a .cmd file, empty: none) runs with "--round <n>", so that a
// reference such as ftpsrv is measured alternately with SMB. Returns the
// number of failures.
int LoadTest(const std::string &ip, const std::string &unc_host, unsigned short port, const prospero::Hello &hello,
             const prospero::Credential &cred, const std::string &folder, std::string *test_dir,
             unsigned mib, int rounds, const std::string &variants, const std::string &round_cmd);

// Idle cost (decision of 24.10.2026): with the share connected, first up to
// 30 s without, then `seconds` with a PING every second; per phase prints
// the CPU time of the server and of this process against the elapsed time.
// Returns the number of phases that missed 0.1 % of a core.
int IdleTest(const std::string &ip, const std::string &unc_host, unsigned short port,
             const prospero::Hello &hello, const prospero::Credential &cred, int seconds);

// Many files in one folder (25.10.2026: deleting 100,000 files left the
// service channel without answers for minutes). In a new test folder below
// <folder>: creates `count` empty files, looks up a sample of them by name
// as the Explorer does before it deletes, lists the folder, and deletes
// what it created, one by one, then the folder. Meanwhile a second
// connection sends a PING every 200 ms. Per phase prints the time per file
// at the start and at the end, the slowest PING and the server's longest
// loop gap. Only files this run created are deleted, by name; the first
// failure stops it. Returns the number of phases with a PING over 1 s.
int ManyFilesTest(const std::string &ip, const std::string &unc_host, unsigned short port,
                  const prospero::Hello &hello, const prospero::Credential &cred,
                  const std::string &folder, const std::string &test_dir, int count);
