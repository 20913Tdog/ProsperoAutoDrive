# Roadmap

As of 2026-10-20. The original plan existed only locally and was deleted. This roadmap was reconstructed from the work history and adapted to the later decisions ([`decisions.md`](decisions.md)). "Release 2.0.0" became 1.0.0 (P6), there is no migration from 1.x, the program always has a tray icon (D4) and is written in C++/Win32 (D5).

Evidence levels: **E1** unit tests on Linux, **E2** protocol CI with a real Windows client, **E3** real PS5.

## Overview

| Step | Question | Status |
|---|---|---|
| S1 | Is port 445 free on the PS5? | passed (E3) |
| S2, S2b | Does libsmb2 serve Windows correctly, and how fast? | passed (E3) |
| S3 | Does writing work? | passed (E3) |
| S4 | Is the throughput sufficient? | passed (E3): reading 96 %, writing 98 % of ftpsrv |
| S5 | Does the server survive rest mode and a restart? | dropped as a live test, simulated in the CI |
| S6 | Can it be tested in the CI with a real Windows client? | passed (E2) |
| S7 | Does discovery work without a firewall prompt? | passed (E3) |
| G1 | Go for native SMB? | **Go** (2026-10-21) |
| **Phase 2** | PS5 server `prospero-smbd.elf` | **starting** |
| Phase 3 | Windows program `ProsperoAutoDrive.exe` | first release 0.1.0 |
| Phase 4 | Hardening | further 0.x |
| Phase 5 | Live acceptance, **G2** | release 1.0.0 |

No product code is written before G1.

## Spikes

### S4 – Throughput

- Read and write one large file, alternating SMB and ftpsrv (port 2121) on the same Wi-Fi. At least 7 runs per direction; the median decides.
- In addition:
  - signing with HMAC-SHA256 (SMB 2.1) versus AES-CMAC (SMB 3.1.1)
  - the CPU time on the PS5
  - the response time for reading a small file during a large copy, because libsmb2 handles requests one after another (risk R4)
- **Pass:** the SMB median reaches at least 70 % of ftpsrv in each direction (G1).
- **File size:** 256 MiB per run, decided on 2026-10-20. The old plan had 2 GiB. On Wi-Fi at 5–8 MB/s one run would have taken about 5 minutes, all 28 runs over 2 hours.
- Procedure and evaluation: [`tests/spikes/README.md`](../tests/spikes/README.md#s4--is-the-throughput-sufficient)
- **Result:** passed. Reading 96 %, writing 98 % of ftpsrv. When reading, the rounds scatter widely (70–119 %). The wait for a small file rises under load from 44 ms to 2.9 s. This is first measured, then fixed in phase 2.

### S5 – Rest mode and restart

**Dropped as a live test (decision of 2026-10-21).** In the CI, scripts simulate the server going away and coming back. Whether the PS5 keeps the payload's connections after rest mode, and whether the payload starts by itself after a restart, will only show in normal operation in phase 5. For that, the project owner sets up an autostart, for example via the autoload of PLDMGR or etaHEN.

### S6 – CI with a real Windows client

- **Windows runner:** the real Windows SMB client against the real server code, as a smoke test.
- **Linux runner:** `mount -t cifs` and a subset of smbtorture.
- **Pass:** both jobs are green on the published commit.
- **Result (E2, 2026-10-21):** passed. The server runs as a static Linux build in WSL 1 and listens on port 4450; the client connects with `/TCPPORT`. `S3-Check.cmd` passes there completely. smbtorture passes 13 of 43 subtests; this list must not get worse. `dev-build` now depends on both jobs. Details: [`tests/spikes/README.md`](../tests/spikes/README.md#s6--can-the-server-be-tested-with-real-clients-in-the-ci)

### S7 – Discovery without a firewall prompt

- A small Windows test program sends, as a standard user, a broadcast request and a unicast ping. The payload answers with its server ID. This is the first Windows code, built with the `windows` preset.
- **Pass:** the answer arrives and no firewall prompt appears.
- **New through variant 1:** the request carries the PC's UTC time, and the payload reports the measured offset. This proves the clock correction at E3 already in S7.

### Gate G1

Go for native SMB if S1, S2, S3, S6 and S7 have passed and S4 reaches at least 70 %. Otherwise the fallback is decided and documented: an own protocol with an own WinFsp client. The file system backend would be reusable.

## Phase 2 – PS5 server

**Status:**

- **P2.1** Skeleton: passed (E1, E2).
- **P2.2** Discovery, pairing, heartbeat: passed (E3, build `8e56644`, 2026-10-22). Unicast and broadcast answer, the PC pairs on the first try, the credentials are in the Windows Credential Manager. All three `PING`s with checksum were accepted, offset 3596.3 s. The test file in `/data/_ProsperoAutoDrive_Test_…` was written and read back right away. No firewall window. The notifications were not observed; "running" is checked along with the P2.3 test.
- **P2.3** Clock correction: passed (E3, build `aed080a`, 2026-10-22). After the restart the stored value of 3596 s applied immediately, and the new test file differed from the PC clock by 0 s. The "running" notification came. With `tools\Connect-Share.cmd` the Explorer opens the share without a password prompt, and the test folders show the correct time (E3).
- **P2.4** Response time under load, measurement 1 (E3, build `a35fdce`, 2026-10-22, 2 rounds of 256 MiB each):
  - Small file without load 63 ms. While writing, median 1.8–1.9 s; while reading, 3.0–3.2 s (max 5.9 s).
  - The service channel on its own TCP connection stayed fast: median 16 ms while writing, 62 ms while reading. So the Wi-Fi itself barely congests.
  - Reading: at most about 1 MiB waits in the server, half in libsmb2's send queue and half in the socket's send buffer. That buffer is 512 KiB, not the 1 MiB requested. At 3.9 MB/s that is about 0.27 s per request. A small file needs several SMB requests in sequence, and each waits again.
  - Writing: nothing waits in the server. The wait arises before the server, on the PC or on the way there.
  - Throughput as in S4: writing 5.3–5.5 MB/s, reading 3.9 MB/s.
  - Not measured yet: unread requests in the PS5's receive buffer, requests per small file, the PC's credits.
  - Limitation: measurement 1 copied with `ReadFile`/`WriteFile` in 1 MiB chunks, so with only one request at a time. The Explorer keeps several open.
- **P2.4** Measurement 2 (E3, build `e2516f4`, 2026-10-22, 5 variants × 2 rounds of 128 MiB each, copy with `CopyFileEx` like the Explorer):

  | Variant | Write MB/s | small ms | Read MB/s | small ms |
  |---|---|---|---|---|
  | Default buffer (512 KiB), credits unlimited | 8.69 | 7969 | 4.69 | 24000 |
  | Buffer 128 KiB, credits unlimited | 8.96 | 9766 | 3.68 | 32062 |
  | Buffer 64 KiB, credits unlimited | 9.13 | 12140 | 2.96 | 41828 |
  | Buffer 128 KiB, credits 64 | 9.09 | 4078 | 3.32 | 23562 |
  | Buffer 128 KiB, credits 32 | 7.60 | 3047 | 2.65 | 26875 |

  - Limitation: the test program copies with `CopyFileEx` without buffering. That the Explorer copies large files the same way is assumed, not measured. A cross-check with the real Explorer and with another SMB server on the same Wi-Fi is still pending.
  - ftpsrv in the same run: writing 8.3 and 9.0 MB/s, reading 4.7 and 2.9 MB/s. So SMB is on par in throughput. Reading fluctuates strongly on Wi-Fi with both methods.
  - Like the Explorer, the PC keeps up to 11 read replies of 1 MiB each outstanding, and up to 8 MiB sit in libsmb2's send queue. A small file therefore waits 24–44 s while reading and 8–12 s while writing, 78 ms without load. Smaller send buffers do not help; the replies then pile up in the send queue.
  - The credit limit shortens the wait while writing to 3–4 s, while reading hardly at all. At 64 it costs no throughput.
  - Measurement 2 was on Wi-Fi.
  - Error: in round 1, writing with credit limit 64 aborted after 7 s (Windows error 59, the PC reset the connection). Before that a variant without limit had run, and the PC held up to 222 credits. With the same limit in round 2 the error did not occur. Suspicion: the limit was lowered from 222 to 64 in the middle of the connection. Not proven yet.
- **P2.4** Measurement 3, **cable** (E3, build `379f98c`, 2026-10-23, the same variants, 128 MiB):
  - Throughput: SMB reads 26–28 MB/s and writes 25 MB/s. ftpsrv reads 114 MB/s and writes 89 MB/s. That is about 24 % and 28 %.
  - Cause: CPU time. While reading, the server computed for 4.1 s out of 4.7 s. libsmb2 signs with a reference AES that recomputes the key for every 16-byte block. Measured locally: 75 MB/s for AES-CMAC with this reference, 1942 MB/s with AES-NI, same results, and all test vectors from RFC 4493 correct.
  - Wait: small file 16 ms without load, 2.7–3.4 s under load, about 1 s while writing with credit limit 32. This matches an 8 MiB queue at 28 MB/s.
  - Error 59 again exactly in round 1, variant 4, at the first lowering of the credit limit in the middle of the connection. The patch now never grants 0 credits when the PC asks for some (MS-SMB2 3.3.1.2).
- **P2.4** Measurement 4, **cable** (E3, build `a4c9e7b`, 2026-10-23, 1 GiB; the output only arrived up to round 1, variant 2):
  - Signing on the PS5: AES-NI active, 1189 MB/s, reference 36 MB/s.
  - Default: reading 115 MB/s (ftpsrv in measurement 3: 114 MB/s), writing 62 MB/s (ftpsrv: 89 MB/s, so about 70 %). Small file under load about 1 s.
  - With credit limit 32 (via `TUNE` in the middle of the connection), writing took 63 s, then reading hung for over 18 min without progress, while small files on the same connection completed in 16 ms. Windows probably waited for enough credits for a large read request. Credit limits are therefore no longer measured; P24 only measures the default. The test program aborts a copy after 60 s without progress.
  - Open: why writing stays at 70 %. For that, the test program now shows per phase the server's CPU time and the time spent in `pread`/`pwrite`.
- **P2.4** Measurement 5, **cable** (E3, build `80f38a9`, 2026-10-23, 3 × 1 GiB, default only):

  | | Round 1 | 2 | 3 | Median | ftpsrv (3 runs) | Share |
  |---|---|---|---|---|---|---|
  | Write MB/s | 57.2 | 80.1 | 78.7 | 78.7 | 89.5–90.0 | 88 % |
  | Read MB/s | 89.7 | 79.5 | 103.5 | 89.7 | 115.6–117.1 | 77 % |

  - Writing: the server spent 6.5 s of 13.4 s per GiB in `pwrite` (about 6 ms per MiB), CPU time 37 %. Receiving and writing ran one after the other.
  - Reading: CPU time only 22–29 %, `pread` 1.4 s per GiB. The send queue was full (7 MiB), but the socket was empty at times: libsmb2 only writes new replies into the socket on the next pass when some are already waiting.
  - Small file under load: about 1 s while writing, 0.05–0.9 s while reading.
  - Measures: `pwrite` of large write requests in a thread of its own; the reply still goes out only after the write (one request at a time, all other requests wait for it). Patch `libsmb2-server-flush`: the server writes every new reply into the socket immediately.
- **P2.4** Measurement 6, **cable** (E3, build `43aa8f1`, 2026-10-24, 3 × 1 GiB):

  | | Round 1 | 2 | 3 | Median | ftpsrv (3 runs) | Share |
  |---|---|---|---|---|---|---|
  | Write MB/s | 65.7 | 76.1 | 77.8 | 76.1 | 73.8 | 103 % |
  | Read MB/s | 111.9 | 107.6 | 107.5 | 107.5 | 114.1–114.9 | 94 % |

  - ftpsrv wrote only 74 MB/s that day (measurement 5: 89 MB/s), and `pwrite` took 8.2 s per GiB instead of 6.5 s. The PS5's SSD was slower.
  - Writing: the write thread took 1024 of 1028 requests, but the main loop waited 7.5 s of 8.2 s for it. Cause: it waited whenever no data was readable at that moment, so also in the middle of receiving the next request. Now it never waits there; the thread wakes it via a pipe.
  - Reading: from 77 % to 94 %; the send queue and the socket were full throughout.
- **P2.4** Measurement 7, **cable** (E3, build `79bf749`, 2026-10-24, 3 × 1 GiB):

  | | Round 1 | 2 | 3 | Median | ftpsrv (3 runs) | Share |
  |---|---|---|---|---|---|---|
  | Write MB/s | 88.9 | 96.9 | 97.6 | 96.9 | 73.4–73.9 | 131 % |
  | Read MB/s | 110.7 | 106.7 | 106.4 | 106.7 | 112.3–113.1 | 95.0 % (exactly 94.98 %) |

  - Writing: the wait for the write thread fell from 7.5 s to 1.6 s per GiB. Now `pwrite` itself is the limit (8.7 s per GiB, almost all system time), and CPU time is at 83–92 %.
  - Reading: unchanged from measurement 6. Send queue and socket full, CPU time 50 %, so the server does not wait. In measurements 6 and 7, round 1 was about 4 % faster than rounds 2 and 3.
  - Assessment: reading is exactly at the limit. With 3 rounds this cannot be told apart from 94 % or 96 %, and ftpsrv only ran after all SMB rounds. P24 therefore now measures as intended for G2: 7 rounds, SMB and ftpsrv alternating, ftpsrv reads the file of the respective round.
- **P2.4** Measurement 8, **cable** (E3, build `86e62c1`, 2026-10-24, 7 × 1 GiB, SMB and ftpsrv alternating, as intended for G2):

  | Round | 1 | 2 | 3 | 4 | 5 | 6 | 7 | Median |
  |---|---|---|---|---|---|---|---|---|
  | SMB write MB/s | 86.1 | 100.0 | 100.0 | 99.9 | 99.9 | 99.9 | 100.3 | 99.9 |
  | ftpsrv write MB/s | 73.5 | 74.1 | 73.9 | 73.5 | 73.9 | 73.5 | 73.7 | 73.7 |
  | SMB read MB/s | 107.0 | 101.5 | 103.3 | 108.4 | 108.9 | 108.7 | 108.7 | 108.4 |
  | ftpsrv read MB/s | 107.7 | 106.8 | 106.0 | 106.8 | 111.0 | 109.6 | 110.2 | 107.7 |

  - **Cable goal reached:** writing 136 %, reading 101 % of ftpsrv (median). Reading per round 95.0–101.5 %, no round below 95 %.
  - This time ftpsrv read only 106–111 MB/s (measurements 5–7: 112–117 MB/s). SMB read as in measurements 6 and 7 (107–108 MB/s). Against the best ftpsrv value of the previous days it would be about 93 %.
  - Small file under load: about 1.0–1.2 s, 15 ms without load. This stays open for later.
  - The goal of the next step was: ≥ 95 % of ftpsrv on cable, see [Decisions](decisions.md#goals-after-measurement-2-on-cable-2026-10-23).

**Scope:**

- Port 445, one share `/`, read and write
- Credentials: transferred during pairing, never displayed
- Configuration, ring log, continuing after rest mode, platform layer
- Discovery over UDP 4445 and service channel on TCP 4445 with server ID, heartbeat, pairing (variant C) and clock correction (variant 1), see [Design phase 2](design-phase2.md)
- **Response time under load:** S4 measured 2.9 s for a small file during a large copy.
  - First measure which buffer accounts for how much of it: libsmb2's send queue (every credit is granted, replies queue at the back), the send buffer of 1 MiB, or Wi-Fi and router.
  - Then fix it specifically: limit credits, move small replies ahead, shrink the send buffer. `S4-Check.cmd` measures the effect.
- Open points from S3:
  - **Security descriptor** (QUERY_INFO type 3): the Explorer asks for it often, over 3000 times in S3. It belongs to the "Security" tab.
  - **Directory class 0x51:** Windows falls back, which costs one extra request per folder.
  - Enforce **share modes and locks**, important for Office and concurrent access
  - **File attributes** such as "Read-only" and "Hidden", ignored so far
  - **Permissions of new files** (root, `0666`): check whether programs on the PS5 cope with them
  - **IPC$** for the share list under `\\PS5`, optional, `P:` does not need it

**Tests (E1/E2):**

- Unit tests of the backend on Linux
- smbtorture suites, among them `smb2.connect`, `smb2.read`, `smb2.dir`, `smb2.rename`, `smb2.setinfo`, `smb2.getinfo`, `smb2.delete-on-close-perms`
- Linux cifs mount with `cache=none` and **fsx**, a random tester for seek, truncate and mixed reads and writes
- **Differential tester** on the Windows runner: the same random sequence of operations on a local NTFS file and on the drive, compared after each step
- Operations as in the Explorer: CopyFile, MoveFileEx, CreateDirectory, SetEndOfFile, overwriting
- 7-Zip extraction with SHA-256
- Fuzzing of the request parser, nightly

**Done when:** all suites are green on exactly the published commit, fsx manages 100,000 and the differential tester 10,000 operations without deviation, and fuzzing runs for at least 1 hour without a crash.

## Phase 3 – Windows program

Design: [`design-phase3.md`](design-phase3.md).

**Status:**

- **P3.1** State machine (`windows/app/core/`): unit tests with a simulated clock and a simulated server (E1, 2026-10-24, local). Drive after at most 2.1 s, no matter when the PS5 appears. Gone after 4.1 s on cable, 13.1 s on Wi-Fi, 2 ms on an orderly end. Dropouts of 2 s (cable) and 9 s (Wi-Fi) do not disconnect. Wrong password: one notification, one attempt per minute. When idle, one wake-up per second (online) or per 2 s (searching).
- **P3.2** `ProsperoAutoDrive.exe` from the command line (E2, 2026-10-24): drive after 74 ms, as `O:`, because a foreign `P:` was in use and stayed untouched. The end of the server was detected immediately, but removing the drive took 5 s, with `WNetCancelConnection2` and with `NetUseDel` alike. In the CI the drive rides on a `net use` connection with another port. Whether this is the same on 445 is measured live by `P32-Check.cmd`. Coming back after a restart is only checked by the live test, because Windows forgets the CI's other port together with the old connection.
- **P3.2** live, **cable** (E3, build `7eb440b`, 2026-10-24, `P32-Check.cmd`):
  - Start: `P:` 0.1 s after the program start.
  - Payload sent again (orderly end): end detected immediately, `P:` removed in 0 ms. The 5 s from the CI do not occur on port 445. Back 2.0 s later, as soon as the new instance ran.
  - Cable pulled: `P:` gone about 3–4 s afterwards. The exact moment of pulling is not known, because the test only asked for Enter after the pull. From now on the log states the age of the last answer.
  - Cable plugged in: `P:` back at the same moment as the service channel. How long the PS5 itself needs after plugging in is not measured.
  - All goals reached: appearing ≤ 3 s, disappearing ≤ 5 s on cable, orderly end < 1 s.

- **Many files in one folder** (P34; live, deleting 100,000 files left the service channel without answers for minutes). Cause: listing called `stat()` for every entry within a single request, and every lookup of a single name read the whole folder. Fixed in `13d3c91`. Live with 20,000 files (E3, build `a6b7ca7`, 2026-10-25, cable): heartbeat at most 16 ms in all phases; creating 138 s (4.5 ms per file at the start, 9.2 ms at the end), single lookups 0.8–1 ms, listing 0.45 s, deleting 118 s (5.8 to 4.4 ms). Creating got slower with the size of the folder, because every new name was searched case-insensitively in the whole folder. With a name cache (`24e4b3e`, valid for 2 s), live: creating 123 s (5.1 → 7.3 ms), deleting 85 s (4.2 ms, constant), heartbeat at most 16 ms. The assumption that the rest of the rise came from rereading every 2 s was wrong: with 30 s (`23d72c3`) live the same, creating 126 s (5.5 → 7.3 ms). The rest is probably in the console's file system, which itself searches the folder for a new name; the server has no influence on that. Back to 2 s, because 30 s brings nothing and sees changes by other programs later.
- **Pairing again after a reinstall** ([decision](decisions.md#pairing-again-after-a-reinstall-2026-10-25)): E1 green, passed live (E3, build `11b3c37`, 2026-10-25). Reinstalled, the notification came, server restarted from the dashboard, "Pair again" paired in 20 ms, `P:` there at once. Before that, an old ELF file from the Payload Manager had been running by mistake. After a new build, the file registered there must be replaced as well.
- **P3.3** Installation and removal passed live (E3, build `b607a8d`, 2026-10-25, project owner):
  - Part A, `C:\Program Files\ProsperoAutoDrive`: entry under `HKLM`, so installed with admin rights. On this test PC there is never a UAC prompt, because `ConsentPromptBehaviorAdmin=0` applies there (elevate without prompting). The program then runs without admin rights (log), `P:` visible in the Explorer, autostart after signing out and in, removal without leftovers.
  - Part B, own folder: installed and removed without admin rights, without leftovers.
  - Open: the visible UAC prompt with default UAC, and a Windows with Smart App Control (phase 4).
- **Stop server** from the tray (signed `QUIT`, the server says `GOODBYE` last) and **Pair with the new server** (after the server was reset): E1 and E2 green, passed live (E3, build `7f97ddc`, 2026-10-25).

**Scope:**

- a single exe, without an installer package and without a ZIP
- Setup on the first start: find the PS5, pair (variant C), store the credentials in the Windows Credential Manager
- State machine: heartbeat over TCP 4445 with the PC time; disconnect only when the heartbeat **and** TCP 445 fail
- temporary mapping `P:`; a foreign mapping on `P:` stays untouched
- Tray icon, notifications, autostart for the signed-in user, ring log
- Removal without leftovers

**Tests (E1/E2):**

- Unit tests of the state machine with a simulated clock and simulated liveness
- CI integration:
  - Server on → `P:` is there after at most 3 s.
  - Server off → `P:` is gone after at most 5 s (cable), 15 s (Wi-Fi), under 1 s on an orderly end of the payload ([decision](decisions.md#immediate-connect-and-disconnect-2026-10-24)).
  - Idle with a connected PC: CPU time under 0.1 % of one core, measured on the PC and on the server.
  - Server on again → `P:` is back.
  - A foreign mapping stays untouched.
  - Wrong password → a notice, but no storm of retries.
  - Server gone for 5 s → no disconnect.

**Done when:** the CI is green, the manifest sets `asInvoker`, the program runs without admin in operation, installing into a protected folder shows exactly one UAC prompt and into a free one none (checked live), and nothing is left after removal.

**Afterwards:** first release 0.1.0 via a PR from `dev` to `main`, after the project owner's live test.

### P33 – Payload as its own process

[Decision](decisions.md#payload-as-its-own-process-2026-10-25): the Payload Manager starts `prospero-smbd.elf` from a file as a process of its own, instead of loading it into `pldmgr.elf` via websend. The file lives in a folder of its own, for example `/data/prospero/` or on the USB stick, never in `/data/homebrew`. The project owner does all steps by hand (E3).

| No. | Step | Expected |
|---|---|---|
| P33.1 | Register in "Manage Payloads" and start from the dashboard | own process under "Active Processes", not under `pldmgr.elf`; `P:` after ≤ 3 s |
| P33.2 | Stop via "Active Processes" | `P:` gone after ≤ 5 s on cable |
| P33.3 | Start again | `P:` back after ≤ 3 s, without new pairing |
| P33.4 | Look under "Active Processes"; pldmgr cannot be stopped because it is part of the jailbreak | own entry with its own PID next to `pldmgr.elf`; when stopping in P33.2, pldmgr keeps running |
| P33.5 | Put it last in "Autoload", restart the PS5 | `P:` comes without any action |
| P33.6 | Copy a new ELF file over the old one via `P:`, stop the server via "Active Processes" and restart it from the dashboard | copying works although the server runs; afterwards the new version runs |
| P33.7 | Start a second time, via autoload and via websend | the new instance ends the old one, as with websend; afterwards exactly one server runs, and `P:` comes back after ≤ 3 s |

**Short probe** (E3, build `bcaa478`, 2026-10-25, cable):

- P33.1 partly: the dashboard shows "prospero smbd" with a green check mark, without a version, and `P:` is connected. The server is **not** listed under "Active Processes", though. Whether it is a process of its own is therefore open.
- P33.6 passed: the file could be overwritten via `P:` while the server was running. Restart from the dashboard: the new instance ended the old one, the program reported the disconnect and reconnected immediately.

**Status:** everything else is developed via websend. The full P33 test runs before the release.

## Phase 4 – Hardening

- **Smart App Control (SAC):** on a Windows 11 with SAC turned on, an unsigned exe does not start at all. The test PC has SAC off (it cannot be turned on again without reinstalling), so P3.3 only checks the case without SAC. With signing, a test on a Windows with SAC must follow, for example in a freshly installed VM.
- **CHANGE_NOTIFY:** the Explorer refreshes itself.
- **Events:** PC standby, Wi-Fi change, IP change
- **Worker threads** for READ/WRITE, only if the measures from phase 2 do not lower the response time under load enough
- **Server-side copy**, seen in S3 as an unsupported IOCTL: otherwise copying within `P:` crosses the Wi-Fi twice.
- **Faster AES-CMAC signing** for wired networks. S4 measured about 20 ms more CPU per MiB than with HMAC. On Wi-Fi it does not slow things down.
- **Leases**, optional, for faster reading. Then it must be proven again that an aborted copy stays visible and nothing is sent afterwards.
- **72-hour endurance test** in the CI with random server restarts
- **Check signing:** SignPath Foundation or Microsoft Store, both free

## Phase 5 – Live acceptance (E3) and release 1.0.0

**To be shortened (2026-10-20):** the project owner considers the scope below too large. A leaner version is still to be agreed.

All write tests run only in the newly created test folder:

1. Explorer operation matrix complete
2. 7-Zip extraction directly onto `P:` with SHA-256 of all files
3. Differential tester with 10,000 operations
4. 10 GiB copy there and back, identical SHA-256
5. 10× PS5 off and on, 3× rest mode, 1× IP change, 2× PC standby, with times from the program's log
6. 7 days of operation without any intervention, proven by the log

**Gate G2:** all points proven with timestamp, commit and a sanitized report, plus a throughput of at least 95 % of ftpsrv, measured as in S4. Then a PR to `main` and release 1.0.0.

## Acceptance criteria 1.0.0

- [ ] Installation without third-party software in at most 2 minutes; path freely selectable (default `C:\Program Files\ProsperoAutoDrive\`), at most one UAC prompt; never admin in operation
- [ ] `P:` appears at most 3 s after the server becomes reachable and disappears at most 5 s (cable) or 15 s (Wi-Fi) after it goes away (median over at least 10 cycles, no outlier over 30 s)
- [ ] Less than 0.1 % of one core of CPU time when idle, on the PC and the PS5
- [ ] No reconnect error at sign-in, no dead drive letter, no foreign drive touched
- [ ] Explorer matrix, 7-Zip, truncate/seek and differential tester passed 100 %
- [ ] 10 GiB copy there and back with identical SHA-256; an aborted copy shows an error, nothing is sent afterwards
- [ ] 7 days of operation with at least 10 PS5 cycles without intervention
- [ ] Throughput at least 95 % of ftpsrv (G2)
- [ ] All tests only in the newly created test folder; `/data/homebrew` untouched
- [ ] CI green on exactly the released commit; binaries with hash and source state
- [ ] Documentation: setup, known limitations, removal

## Open across phases

- **Clock:** measurement from 2026-10-26 whether the offset disappears with the end of daylight saving time ([decision](decisions.md#clock-skew-the-pc-provides-the-reference-time))
- **libsmb2:** report bugs found upstream, only after the project owner's approval
- **Windows 10 22H2:** best effort, check live at least once before 1.0.0
- **Risks:**
  - Microsoft is phasing out NTLM in the long run. The backend is protocol-neutral, and libsmb2 can do Kerberos.
  - Company policies on managed PCs. The target group is private PCs; errors are reported clearly.
  - Changes to firmware or the exploit chain. Only standard SDK calls are used.
  - Crash of the payload. Fuzzing, an outer restart loop and the log help against it.
