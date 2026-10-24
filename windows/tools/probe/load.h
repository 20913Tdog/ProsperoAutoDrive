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
