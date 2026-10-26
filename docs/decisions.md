# Decisions

This file records what was decided and why, so that it can be traced later. The project owner makes the decisions. The binding rules for the work are in [`CLAUDE.md`](../CLAUDE.md).

New entries are added at the bottom. If a decision is changed, the old entry stays and gets a pointer to the new one.

The original planning document with the decision log of 2026-10-20 existed only locally and was deleted. The entries below were reconstructed from the work history.

## Restart, 2026-10-20

### Architecture: native SMB (D1)

The old stack of FTP, rclone and WinFsp (v1.x) is discarded. A payload of our own on the PS5 provides an SMB2/3 server. Windows uses only its built-in SMB client and a small helper program.

- **Why:** FTP cannot deliver direct writing and the full Explorer semantics at the same time, because rclone's VFS has limits. In addition, ftpsrv answers MKD, RMD, RNTO and DELE with 226 instead of 257 or 250. Native SMB needs no driver and no third-party software on Windows.
- **Rejected:**
  - **Repair the old stack:** the target picture would remain out of reach.
  - **NFSv3:** the Windows client only has soft mounts with short timeouts, blocks up to 32 KB, and is missing on Home.
  - **WebDAV:** the WebClient service is deprecated and has a file limit of about 50 MB.
  - **SFTP:** there is no SSH server on the PS5, and WinFsp would still be needed.
- **Fallback:** an own protocol with an own WinFsp client, in case native SMB fails at gate G1. The file system backend would be reusable.
- **Basis:** the server engine of libsmb2 (LGPL-2.1, statically linked, pinned to a commit as a submodule) plus an own POSIX backend. Because of the PS5 SDK, the payload is under GPL-3.0.

### Windows versions (D2)

Windows 11 is fully supported: Home, Pro and Enterprise with default security. Windows 10 22H2 is best effort, because it has had no regular support since 10/2025.

### Share and drive (D3, D3a, D3b)

- One share, root `/`, read and write. It covers `/data` and the extended storage.
- One drive `P:` points to the root. Instead of dynamic shares there are subfolders.
- **No protection list** for system paths, as with FTP. The risk is known and deliberately accepted.

### Security

NTLMv2, signing always on, no oplocks and no leases, synchronous writes. Windows 11 Pro from 24H2 requires signing and blocks guest access, so there is no guest access.

### Windows program (D4, D5, D8)

- **Visibility:** there is always a tray icon with status and menu. Notifications come on connect, disconnect and problems (P9).
- **Technology:** C++ and Win32, statically linked, MIT license. The program uses temporary drive connections and keeps the credentials in the Windows Credential Manager. It needs no admin and no driver.
- **Signing:** unsigned for now, because there is no budget. The program stays **generic** and contains no PS5 or jailbreak functions. This keeps the free routes open: SignPath Foundation or Microsoft Store. This is checked after the first 0.x release.
- **No longer valid:** D5 originally said "MSVC in CI". Replaced by [Toolchain](#toolchain-llvm-mingw-instead-of-msvc).

### Connecting and disconnecting (P8)

The PS5 is on Wi-Fi. The program checks over UDP with a server ID whether the PS5 is reachable. It disconnects only when this check **and** a test on TCP port 445 fail. Wi-Fi dropouts are a test case of their own. Port 445 is the primary path.

### Throughput gates (D7)

- **G1** (after the spikes): ≥ 70 % of ftpsrv
- **G2** (release 1.0.0): ≥ 95 % of ftpsrv

Measured on Wi-Fi, at least 7 alternating runs per direction; the median decides.

### Procedure

The spikes S1–S7 and gate G1 run before any product code.

### Repo, branches and releases (P1–P7, P11, P12)

- **P1:** the old GitHub repo was deleted and created anew, empty.
- **P2:** old documents are not carried over.
- **P3, P4:** no binaries in Git. The CI builds every commit on `dev`; a green build replaces the pre-release `dev-build`. `main` is stable and changes only via PR after a live test and the project owner's OK.
- **P5:** a release publishes the hash-identical artifacts of the tested `dev` build. Nothing is rebuilt.
- **P6:** versions 0.x until gate G2, then 1.0.0. There is no migration from 1.x.
- **P7:** name "Prospero AutoDrive", technically `ProsperoAutoDrive` and `prospero-smbd`.
- **P11:** the repo is public. This brings free Actions minutes, protection for `main` on the Free plan and a CI that can be read without signing in. It is also a prerequisite for SignPath.
- **P12:** commits only with the GitHub noreply address, never with a work e-mail address.
- **Changed:** P3 planned the pre-release "dev". It is now called `dev-build`, because a tag `dev` next to the branch `dev` would be ambiguous.
- **Superseded:** P2 planned to keep old documents only in the local archive. That archive, with the backup of the old repo, was deleted. Since then this file exists.

### Way of working (P10)

All changes go into the repo, because testing happens on another PC. Assumptions that deviate from the norm are actively questioned, including those of the project owner.

### Toolchain: llvm-mingw instead of MSVC

One toolchain for all targets, locally and in the CI with the same versions:

- llvm-mingw `20260922` for PS5 and Windows
- ps5-payload-sdk `v0.44`
- CMake and Ninja

**Why:** the development machine has neither WSL nor admin rights. Visual Studio Community would be free for open source, but it is not needed this way. On Windows, the SDK's compiler wrappers add `crt1.o` twice. Therefore `clang` is called directly.

## Results of the spikes, 2026-10-20

### S1: port 445 is free

The payload binds 445 with effective root rights, and Windows reaches the port over Wi-Fi. An alternate port is not needed.

### S2: libsmb2 serves Windows correctly

Reading, everything arrived correctly:

- NTLMv2 with signing works on Windows 11 Pro 25H2.
- All checksums match.
- The Explorer shows nothing unusual.
- The missing SPNEGO mechListMIC does not bother Windows, only Samba's `smbclient`.

### S2b: TCP_NODELAY and large socket buffers become the default

- With 1 MiB socket buffers and TCP_NODELAY (profile b), small files go 2.5 times faster.
- SMB 3 signing (AES-CMAC) does not slow things down on Wi-Fi yet. For G2 it will be accelerated.
- SMB 3.1.1 stays.
- The PS5's internal UTC clock is 1 hour ahead. This matters for timestamps in S3.

### S3: writing works

Passed in the second run (E3, build `46a55e4`):

- All bytes, all modification times and all 13 file operations are correct. Among them are changing only the case of a name, moving and recursive deletion.
- Writing is as fast as with ftpsrv (one run, 64 MiB each): SMB 7.7 MB/s, FTP 7.2 MB/s.
- New files belong to root and have the permissions `0666`.
- New files without a given time get the PS5's time and are therefore 1 hour in the future. The kernel reports daylight saving time of 60 minutes for this.
- **Resolved:** the reference time comes from the PC, see [Clock skew](#clock-skew-the-pc-provides-the-reference-time).

### S4: the throughput is sufficient, G1 passed

Passed (E3, build `299cb3d`, Wi-Fi, 7 alternating runs per direction with 256 MiB, median):

- Reading: SMB 3.90 MB/s against FTP 4.06 MB/s, so **96 %**. Compared per round, the ratio scatters from 70 to 119 %, median 100 %.
- Writing: SMB 7.10 MB/s against FTP 7.20 MB/s, so **98 %**. Per round 96 to 101 %.
- When writing, SMB is on par with FTP. When reading, also in the median, but with 7 rounds only accurate to about ±8 %.
- **SMB 3.1.1 stays.** It reaches almost the FTP values anyway. The comparison with SMB 2.1 (HMAC) ran only 3 times and without an FTP run next to it.
- **CPU:** AES-CMAC costs about 20 ms per MiB more than HMAC-SHA256. The whole server needs 36 to 39 ms per MiB in one thread. So it reaches at most about 26 to 27 MiB/s. On Wi-Fi that does not matter, on a wired network it does. The fast AES-CMAC stays earmarked for phase 4.
- **File size:** 256 MiB instead of 2 GiB per run. With 2 GiB the test would have taken over 2 hours on Wi-Fi. The proposal came from the developer; the project owner agreed with "go on with S4".
- **Open: wait under load.** A small file takes 2.9 s instead of 44 ms during a large copy. That does not count for G1, but it would be noticeable in the Explorer.
  - **Proven in the code:** libsmb2 grants every requested credit, keeps reading without a brake and queues replies at the back.
  - **Not proven:** which buffer accounts for how much of the wait. Candidates are the send queue, the send buffer of 1 MiB, and Wi-Fi and router.
  - First measure, then fix (see S4 in the spike README).
- **G2 (≥ 95 %):** G2 cannot be read reliably from this measurement. When writing, SMB is clearly above 95 %. When reading, the rounds scatter too much; that needs more rounds or the median of the per-round ratios. It is evaluated only on the build that becomes 1.0.0 anyway.

Gate G1 is therefore not decided yet, because S6 and S7 are still missing.

### S5 dropped as a live test (2026-10-21)

Rest mode and restart of the PS5 are not tested live as a spike of their own. Instead, scripted runs in the CI simulate the server going away and coming back: stop, restart, network gone.

- **Why:** the project owner does not consider an active test necessary for this.
- **What the simulation does not cover:**
  - whether the PS5 keeps the payload's connections after rest mode
  - whether the payload starts by itself after a restart

  This only shows in normal operation in phase 5. The program's log proves it there.
- G1 did not depend on S5 anyway.

### S6: the real Windows client runs in the CI (2026-10-21)

Passed (E2). On `windows-2025`, the real SMB client of Windows passes the complete `S3-Check.cmd`, the same script as live on the PS5. The Linux kernel client (cifs) reads and writes; a selection from smbtorture runs as well.

- **Server on the Windows runner:** the static Linux build in WSL 1 with a minimal Alpine rootfs, pinned by checksum.
  - **Why:** it is the same code and the same POSIX semantics as in the Linux tests, without a Windows layer of its own for the backend.
  - **Rejected:**
    - **Windows build with llvm-mingw:** that would need a Windows layer just for tests, and NTFS behaves differently from the PS5's file system.
    - **MSYS2:** NTFS without case sensitivity, plus a second toolchain.
    - **A ready-made action for WSL:** that would be third-party code in the CI. `wsl --import` is enough.
- **Port:** in the CI 4450 with `/TCPPORT`, because Windows itself occupies port 445. Port 445 remains a matter for E3.
- **smbtorture:** 13 of 43 subtests pass. They are listed in `tests/spikes/smbtorture-pass.txt`. If one of them fails, the CI fails. The other 30 are work for phase 2.
- **From now on** `dev-build` is only published when both client jobs are green.

### S7: the PS5 is found without a firewall prompt (2026-10-21)

Passed (E3, build `ff9d50a`, Windows 11 25H2, standard user, firewall profile "Public"). The unicast part runs in the CI as well (E2).

- Unicast and broadcast over UDP are both answered, and no firewall window appears. The program binds no fixed port; it sends requests and reads the replies to them.
- **Variant 1 works:** the server measures its offset from the PC clock to about 0.1 s, here 3596.5 s.
- **For the program this means:** the PS5 is found via the stored IP. If the IP changes, the broadcast helps, and the server ID tells whether it is the same PS5.
- **Still open for phase 2:** the final UDP port (4446 in the spike) and a permanent server ID instead of a random one per start.

## Gate G1: go for native SMB (2026-10-21)

The project owner approved G1. S1 to S4, S6 and S7 were met; S5 was dropped. The fallback (own protocol with WinFsp) is not needed. From now on product code is written, starting with phase 2 (PS5 server `prospero-smbd.elf`).

- **Known and open:**
  - the wait under load (2.9 s)
  - 30 of 43 smbtorture subtests
  - the daylight saving time check from 2026-10-26
  - rest mode and restart in normal operation
- **The project owner's note on the password:** a PS5 notification disappears and does **not** show up in the list of notifications afterwards. A password shown only once that way is therefore not enough for the product. The route to the password is decided in the design for phase 2.

## Phase 2, fundamental decisions (2026-10-21)

### Service channel over TCP 4445 instead of UDP

Discovery, heartbeat, time and pairing run over a TCP connection on port **4445**, not over UDP.

- **Why (project owner):** TCP is connection-oriented.
- **Consequences:**
  - There is no broadcast with TCP. Discovery therefore stays with UDP, see the addition below. Outgoing TCP connections trigger no firewall window. Every test run with `curl.exe` on port 4445 showed that as well.
- **Changes:**
  - [Connecting and disconnecting (P8)](#connecting-and-disconnecting-p8): instead of the UDP liveness there is a heartbeat over TCP 4445. It still disconnects only when the test on TCP 445 fails as well.
  - [Clock skew](#clock-skew-the-pc-provides-the-reference-time): the PC time comes over this channel instead of over UDP.
- Port 4445 used to be the control port of the test payloads. In the product it belongs to the service channel.
- **Added the same day (project owner):** discovery continues over **UDP 4446**, as in S7 with unicast and broadcast. The answer gives server ID, name and the ports. Everything else runs over TCP 4445. This removes the need to scan the network.
- **Changed the same day:** discovery runs on **UDP 4445** instead of 4446, see [Ports and discovery sequence](#ports-and-discovery-sequence).

### Ports and discovery sequence

The PS5 binds **TCP 445** (SMB), **TCP 4445** (service channel) and **UDP 4445** (discovery). TCP and UDP have separate port numbers; the same number does not clash.

- **Sequence in the program:**
  1. In everyday use it connects via TCP 4445 to the stored IP. If the same server ID answers there, the PS5 is found.
  2. Only if that fails, for example during setup or after an IP change, does it search via UDP broadcast.
  3. As a last resort the user enters the IP by hand, for example when the router filters broadcasts.
- **Why:** asked by the developer for a sensible design, the project owner agreed to this proposal with "then go ahead". One number for both is easier to remember and to allow in a firewall. A router or mesh system that filters broadcasts then only disturbs the first search.
- **Limit:** a guest network or client isolation in the router separates PC and PS5 entirely. Then no choice of port helps.
- **Replaces:** UDP 4446 from the addition above. Spike S7 stays at 4446, as it was measured.

### Pairing without a code (variant C)

The password is never displayed or typed. The first PC that reports over the service channel after the first start gets user and password. The PS5 then reports "paired with …".

- **Why (project owner):** on a private Wi-Fi everyone is responsible for their own security. Anyone doing such a thing on a public Wi-Fi is acting carelessly anyway. The user deliberately bears the risk that another device on the Wi-Fi pairs first.
- **Rejected:**
  - **A, show the password repeatedly:** a long password is tedious to type.
  - **B, pairing with a code:** more effort, without the project owner needing the protection.
- When pairing is open and how a second PC joins is described in the [design for phase 2](design-phase2.md).

### Design phase 2 confirmed

The project owner confirmed the proposals from the [design for phase 2](design-phase2.md), with one change:

- **Share `prospero`** instead of `PS5`, so `\\<PS5 IP>\prospero`.
- **Pairing:** the first pairing stays open until a PC is paired. Another PC joins via "Pair another PC" on a paired PC; pairing is then open for 10 minutes. Resetting is done by deleting `config.ini`.
- **Heartbeat:** every 5 s. The PS5 counts as gone after 15 s without an answer and a failed test on TCP 445.
- **Clock:** the offset is only taken from heartbeats with a valid checksum (HMAC-SHA256 with the password).
- **Folder** `/data/prospero-smbd/` for configuration and log.

### Rule: only libkernel functions on the PS5

In `libc.a`, the SDK contains a bare call for every FreeBSD system call number. These calls do not set `errno`, and on the PS5 a number can mean something else. In S3, `utimensat()` therefore did nothing.

- **Rule:** on the PS5, only use functions that libkernel exports. The CI checks this (`.github/scripts/check-raw-syscalls.sh`).
- **Likewise:** new library functions only count as safe once they have run on the PS5. After `localtime_r()` the payload went silent.

## Goals after measurement 2 on cable (2026-10-23)

The project owner connected the PS5 by cable (a fixed IP) in order to work under the best conditions first and to optimize for Wi-Fi later.

- **Primary goal of the next step:** on cable **≥ 95 % of the speed of ftpsrv**, reading and writing.
- **Afterwards:** the connection is set up and torn down immediately when the state of the PS5 changes (phase 3). So far, from the design of phase 2: heartbeat every 5 s, gone after 15 s without an answer and a failed test on TCP 445. What "immediately" means in seconds is defined there.
- **State on cable (E3, build `379f98c`):** SMB reads 26–28 MB/s and writes 25 MB/s, ftpsrv 114 and 89 MB/s, so about 24 % and 28 %. While reading, the server was computing 87 % of the time. libsmb2 signs with a reference AES that recomputes the key for every 16-byte block. On Wi-Fi this did not show, because the radio link was slower than this limit; G1 was measured on Wi-Fi.
- **Measure:** AES-CMAC with AES-NI (patch `libsmb2-cmac-aesni`); signing always stays on. At start, the server checks the test values from RFC 4493 and compares with the reference; otherwise it uses the reference.
- **Gates G1/G2** stay defined in the text as Wi-Fi measurements until the project owner changes that. From now on, every measurement states Wi-Fi or cable.

### Result (2026-10-24)

Cable goal reached (E3, build `86e62c1`, 7 rounds alternating): writing 136 %, reading 101 % of ftpsrv, no read round below 95 %. Details in measurement 8 in the [roadmap](roadmap.md).

## Immediate connect and disconnect (2026-10-24)

The project owner accepted the proposal:

- **Appearing:** `P:` is there at most **3 s** after the PS5 becomes reachable. As long as the PS5 is missing, the program tries to connect every second.
- **Disappearing on cable:** at most **5 s**. There is a heartbeat every second. It disconnects after 3 missed ones, and only if the test on TCP 445 fails as well.
- **Disappearing on Wi-Fi:** 10–15 s, because short dropouts are normal there and `P:` should not disappear in the middle of a copy.
- **Orderly end of the payload:** it signs off over the service channel; `P:` is gone after less than 1 s.
- **The project owner's condition:** this must not cost noticeable CPU on either the PS5 or the PC, "completely relaxed for both machines". This is measured, not assumed:
  - **PC:** the program sleeps in a wait state between heartbeats; it does not poll in a loop. The goal is CPU time below 0.1 % of one core and no wake-up more often than once per second.
  - **PS5:** a heartbeat is a line of about 100 bytes per second, without signature computation over large data. The goal is CPU time of the payload below 0.1 % of one core when idle with a connected PC. Today the server loop wakes every 100 ms without load (libsmb2's default). This is measured first and only lengthened if it measurably causes load.
- **Open:** how the program tells whether cable or Wi-Fi applies. The PC only knows its own connection, not the PS5's. Proposal: the PS5 reports its connection type in the `HELLO`, if it can determine it. Otherwise the cautious Wi-Fi time applies.
- **Measurement P30, cable** (E3, build `aef1906`, 2026-10-24, share connected, nothing transferred):
  - Without heartbeat, 30 s: server 0.067 % of one core, PC 0.000 %. That is the server loop waking every 100 ms.
  - Heartbeat every second, 120 s: server 0.080 %, PC 0.038 % (with the test program's progress bar). All 120 `PING`s answered. So the heartbeat itself costs about 0.013 % on the PS5.
  - Goal below 0.1 % reached. The PS5 has 8 cores; relative to the whole console that is about 0.01 %.
  - Connection type: `getifaddrs` works on the PS5. On cable the interface is called `eth0`, type Ethernet. `SIOCGIFMEDIA` returns nothing. The Wi-Fi interface only appears when it is connected.
- **Measurement P30, Wi-Fi** (E3, build `b56cd09`, 2026-10-24): the interface is called `wlan0` and also reports type Ethernet. So the name decides: `eth…` is cable, `wlan…` is Wi-Fi. Idle: server 0.037 % without and 0.045 % with heartbeat, PC 0.038 %. The heartbeat came back after 4–5 ms (cable: 0–1 ms).
- **Implementation:** the payload states the connection type in the `HELLO` (`link=wired|wifi|unknown`). With `wired` the program uses the cable time, otherwise the Wi-Fi time.

## No automatic broadcast (2026-10-24)

The project owner does not want to burden the home network with broadcasts when the PS5 is simply off. The first proposal was a dialog after 30 s without an answer. Against it: the PS5 is usually off, so the dialog would come after every PC start, and the program cannot tell "off" from "new IP". The route via the tray was decided:

- In everyday use only connection attempts to the stored IP, every 2 s.
- The tray icon shows "not reachable, last seen …".
- A broadcast only runs via "Search the network for PS5 (30 s)" in the tray menu and once during setup.

## Drive, installation, removal (2026-10-24)

- **`P:` in use:** the foreign drive stays untouched. The program takes the next free letter backwards from `O:` and reports this once.
- **Installation:** an installation dialog with a selectable target folder, no silent self-copy. Afterwards one never has to touch the program again. The exe is its own installer; there is no separate installer package.
- **Admin rights allowed for the installation** (project owner, 2026-10-24; changes "needs no admin" from D5 for the installation):
  - The installation path is freely selectable, preset to `C:\Program Files\ProsperoAutoDrive\`, explicitly not `%LOCALAPPDATA%`. If the chosen folder needs admin rights, the installer itself asks via UAC. A right-click "Run as administrator" is not necessary.
  - **The program itself keeps running without admin.** A drive that a process with admin rights connects is not visible to the normal Explorer. That is a property of Windows, not a choice.
  - Without a signature, the UAC dialog shows "Unknown publisher" until signing (phase 4) is in place.
- **Removal:** via Windows (Installed apps / "Uninstall a program"), through an entry for the user under `HKCU`.

## Pairing again after a reinstall (2026-10-25)

A removal deletes the credentials on the PC, but the server still lists the PC as paired and then lets no one pair any more. Decided (project owner): restarting the server in the Payload Manager is the way back.

- After every start, pairing is open for 120 s until a PC pairs. An already paired PC does not change that.
- After a refusal the program tries again every 10 s and thus pairs by itself shortly after the restart.
- If that does not work, it reports once "found, but pairing is not possible (probably an old pairing). Restart the server, then 'Pair again' in the tray". "Pair again" tries immediately.
- Risk: in the 120 s after every start, including autoload, another device on the home network can pair. The PS5 reports that with "paired with …".

## Payload as its own process (2026-10-25)

Noticed live: sent over the network to the Payload Manager (pldmgr), `prospero-smbd.elf` runs inside the process `pldmgr.elf`. It cannot be stopped on its own, and it dies with pldmgr. Payload Manager, on the other hand, starts payloads that are registered as a file under "Manage Payloads" as processes of their own and can load them at start via "Autoload".

- **Route:** register in Payload Manager plus autoload; no loader of our own and no tile of our own. The tile of ps5-web-file-manager is only a bookmark to its own web interface, so it cannot switch the server on and does not resolve the dependency on pldmgr. It is postponed.
- **Sequence** (project owner): now a short probe (P33.1, P33.4, P33.6), to see differences of the own process early. Then continue developing via websend until all functions are in place, because that is faster. The full P33 test comes at the end, before the release.

## Test procedure, 2026-10-20

### Test with one click

The project owner only starts `tools\Run-Test.cmd` and sends the output. The script runs like this:

1. It fetches the source state and the checked build. The build must match exactly this state.
2. It restarts the payload, via the ELF loader on port 9021 or by hand in the Payload Manager.
3. It fetches the password once via the control port.
4. It runs the test and puts the output on the clipboard.

- **Trade-off:** the test payload hands out its password exactly once, never again afterwards. Another device on the Wi-Fi could fetch it first. It would then only get at the newly created test folder with generated test data, for at most 15 minutes (S2, read only) or 30 minutes (S3, also write). The control port was reachable without a password before as well. This does not go into the product.
- **Rejected:** remote-controlling the Payload Manager (PLDMGR) via its interface. That would have written into its existing folder on the PS5, and tests may only write in newly created test folders. In addition, the interface is foreign and unofficial.

## Time, 2026-10-20

### Clock skew: the PC provides the reference time

The PS5's internal UTC clock is 1 hour ahead (S2b, S3). Variant 1 was decided:

- The Windows program sends the PC's UTC time along in every UDP liveness message.
- From this, the server measures the offset of its clock and converts all timestamps. When reporting to Windows it subtracts the offset; when Windows sets a time it adds it. On disk, the times are stored as the PS5 itself writes them.
- The program stays generic, because a time value in a liveness message is nothing PS5-specific.
- **Why:** it works without internet, and many jailbroken PS5s are offline. The clock of a Windows PC is normally set by the time service.
- **Rejected:**
  - **The kernel's daylight saving time value (`tz_dsttime`):** its meaning on the PS5 is not proven.
  - **NTP on the PS5:** needs internet.
- **Open for phase 2:**
  - Round the offset and only adopt it on a clear change, so that times do not jump by seconds.
  - Only trust genuine messages, for example only from a PC with a signed-in SMB session or with a checksum over the password. Otherwise any device on the Wi-Fi could shift all times.
  - Define what applies before the first message, for example the last measured value.
- ~~**To be checked from 2026-10-26:** whether the daylight saving time is in the kernel clock.~~ **Resolved 2026-10-25 (E3):** the hour came from the PS5's settings, not from the kernel. The time zone was set to London while the shown time had been set by hand to the time in Berlin, so the internal UTC was 1 hour ahead (`tz_minuteswest=0`, `tz_dsttime=60` fit this). After setting the time zone to "UTC+01:00 Berlin" and the time by hand to the wall time, the offset was -40 s, after a second, more careful setting -2 s. The PS5 has no time sync without the internet, so a few seconds of drift remain; the measured offset corrects them as before. No change to the code.

## Release 0.1.0: answers to the open questions (2026-10-26)

The project owner answered the questions Q1–Q6 of the [release plan](release-0.1.0-plan.md):

- **Q1, timer fix:** goes into 0.1.0 (T1). It is tested together with P33 on one build.
- **Q5, place of the ELF file on the PS5:** `/data/prospero-smbd/`, the same folder as `config.ini` and the log. The server writes only its own files there (`config.ini`, `config.ini.tmp`, `config.ini.bad`, `log.txt`, `log.1.txt`), so the ELF file next to them is never touched. Removing everything from the PS5 is one folder. Resetting the server means deleting `config.ini` only, not the folder, or the Payload Manager no longer finds the file. This replaces the example `/data/prospero/` in [P33](roadmap.md#p33--payload-as-its-own-process); it is still a folder of its own and never `/data/homebrew`.
- **Q2 and Q6, release files:** `ProsperoAutoDrive.exe`, `prospero-smbd.elf`, `SHA256SUMS`, `BUILD-INFO.txt`, the license texts, `THIRD-PARTY-NOTICES.txt` and a complete source archive including the libsmb2 submodule (GPL-3.0 of the payload). No test programs, no spike payloads.
- **Q3 and Q4, kind of release and who starts it:** a normal release, not a pre-release, so that `/releases/latest` works; the 0.x version already says that it is early. The project owner starts it with "Run workflow" in the Actions tab; that click is the go.

## Timers on the monotonic clock (2026-10-26)

Setting the PS5's clock while the server runs, as the project owner did on 2026-10-25, moved every timer that used the wall clock. Moved back by 1 h, the pairing window after a start stayed open for about 1 h instead of 120 s, and idle service connections were closed only after about 1 h instead of 60 s. Moved forward, the pairing window closed at once and every idle connection was dropped at once.

- **Decided (T1, Q1):** timers and durations use `CLOCK_MONOTONIC` (`ps5/smbd/monotime.h`): the pairing windows, the idle timeout of the service channel, `uptime_s`, the outer restart loop and the loop gap of the queue diagnosis. `CLOCK_MONOTONIC` was already proven on the PS5 (name cache, signing check).
- **Stays on the wall clock:** the time stamps of the log, `now_utc` in the status, the clock skew from `PING` (it compares the PC's UTC with the console's), and file times.
- **Test:** host test case 6b moves the wall clock of the server process 2 h forward with libfaketime and leaves the monotonic clock alone.
