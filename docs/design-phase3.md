# Design phase 3: Windows program `ProsperoAutoDrive.exe`

As of 2026-10-24, **proposal, not confirmed yet**. It is based on [`decisions.md`](decisions.md), above all [immediate connect and disconnect](decisions.md#immediate-connect-and-disconnect-2026-10-24), and on the service channel from the [design phase 2](design-phase2.md).

## Goal

A single exe that is set up once and never has to be touched again afterwards:

- If the PS5 is reachable, the drive `P:` appears after at most 3 s.
- If it is gone, the drive disappears after at most 5 s on cable or 15 s on Wi-Fi, and after less than 1 s on an orderly end of the payload.
- When idle, this costs less than 0.1 % of one core on PC and PS5 (P30 measured: PC 0.038 %, PS5 0.045–0.080 %).
- Always a tray icon, notifications on connect, disconnect and problems.
- In operation no admin, no driver, no third-party software, no PowerShell. Only the installation may ask for admin rights, if a protected folder is chosen. The program stays **generic**: it only knows "a server with a service channel and an SMB share", nothing PS5-specific. Name, share and ports come from `HELLO`.

## States

```
            ┌──────────────── error / server gone ────────────────┐
            v                                                      │
 SEARCH ──TCP 4445 ok, HELLO, server ID matches──> PAIR? ──> CONNECT ──> ONLINE
   ^  every 2 s an attempt to the stored IP          (once only) (create P:)  │ PING every s
   │  broadcast only on request (tray menu)                                   │
   └──────────── P: removed <── CHECK (TCP 445) <── heartbeat missing ────────┘
```

| State | What happens | Next |
|---|---|---|
| **Search** | Every 2 s a connection attempt to the stored IP, 1 s time limit. **No automatic broadcast** (decision of 2026-10-24): the tray icon shows "not reachable, last seen …"; the broadcast only runs via "Search the network for PS5 (30 s)" in the tray menu and once during setup. | Service channel is up and the server ID matches |
| **Pair** | Only the first time, when no credentials are stored: `PAIR`, credentials into the Credential Manager | Connect |
| **Connect** | `WNetAddConnection2` with `CONNECT_TEMPORARY` on `P:` | Online, or on error a notice and Search with a pause |
| **Online** | `PING` with checksum every second, contains the PC time | Answer missing: Check |
| **Check** | After 3 missed answers (cable, `link=wired`) or 12 (otherwise): connection attempt on TCP 445, 1 s time limit. If the PS5 closes the service channel, check immediately. | 445 answers: stay Online, set up the channel again. Otherwise remove `P:`, Search |

Calculation for cable: 3 s missed + at most 1 s test + disconnect ≈ 4–5 s. Orderly end: the payload closes the channel, the other side notices that immediately, the test on 445 is refused immediately, so less than 1 s.

**Further triggers:**
- The PC wakes from standby, the network changes (`NotifyIpInterfaceChange`): Check or Search immediately, not only after the next tick.
- Sign-out, shutdown: remove `P:`.

## Relaxed for both machines

- One thread waits with `WaitForMultipleObjects` for the socket (`WSAEventSelect`), a waitable timer and system events. No polling in a loop, no wake-up more often than once per second.
- A `PING` is about 150 bytes there and 100 back.
- If the PS5 is off, the PC sends a connection attempt into its own network every 2 s. That is a single packet and burdens nothing.

## Drive letter

- Default `P:`. If `P:` is in use (foreign mapping, USB stick), it is **not** touched.
- The program then takes the next free letter backwards from `O:` and reports this once (confirmed on 2026-10-24).

## Setup, autostart, removal

- **Installation dialog** (decision of 2026-10-24): if the exe does not start from its installation folder, it shows a dialog. Afterwards one never has to touch it again.
  1. **Target folder freely selectable**, a field with "Browse…", preset to `C:\Program Files\ProsperoAutoDrive\` (decision of 2026-10-24, not `%LOCALAPPDATA%`). If the folder needs admin rights, the installer itself asks via UAC, only for the copying and the entries. A right-click "Run as administrator" is not necessary.
     - **The program itself always runs without admin**, also after an installation for everyone. A drive that a program with admin rights connects is not visible to the normal Explorer. Therefore the installer explicitly starts the program without admin rights at the end.
     - **Autostart and uninstall entry:** under `HKLM` if the installation ran with admin rights, otherwise under `HKCU`. If the installer wrote under `HKCU` with admin rights, the entry would end up with another account in case a standard user enters an admin's password.
     - Credentials, settings and log always stay with the respective user.
  2. Check boxes "Start with Windows" (the user's autostart, `HKCU\...\Run`) and "Start menu entry", both preset.
  3. The program copies itself into the folder, creates the uninstall entry and starts from there.
  4. It searches for the PS5: first broadcast, then entering the IP. Then it pairs and stores IP, server ID and credentials.
  - If a newer exe is started, the dialog offers the update in the same place.
- **Tray menu:** status in the first line (e.g. "PS5 connected as P:, cable" or "PS5 not reachable, last seen …"), "Open drive", "Search the network for PS5 (30 s)", "Pair another PC", "Start with Windows" (on/off), "Exit".
- **Uninstall via Windows** (decision of 2026-10-24): under Settings → Apps → Installed apps or "Uninstall a program". The entry is under `...\CurrentVersion\Uninstall\ProsperoAutoDrive`, in `HKLM` for an installation with admin rights (then the removal also asks via UAC), otherwise in `HKCU`.
  - The removal ends a running program and removes `P:`, autostart, start menu entry, credentials, settings and log, then the program folder.
  - The running exe cannot delete itself. A short `cmd.exe` call takes care of that, waiting until the program has ended (no PowerShell).
- **Settings:** `%LOCALAPPDATA%\ProsperoAutoDrive\settings.ini` (IP, server ID, letter), ring log next to it. The password is only in the Credential Manager.

## Notifications

Via the tray icon (`Shell_NotifyIcon`), without a third-party library:

- "PS5 connected as P:", "PS5 disconnected"
- Problems: sign-in refused, `P:` in use, pairing refused. Each once, not on every attempt. A wrong password does not lead to a storm of retries: afterwards it is only tried every 60 s.
- Short Wi-Fi dropouts below the disconnect time report nothing.

## Structure of the code

| Part | Content | Test |
|---|---|---|
| `windows/app/core/` | State machine without Windows calls: inputs are events and the time, outputs are actions ("connect channel", "create P:", "report …") | E1: unit tests with a simulated clock in the preset `host`, also on Linux |
| `windows/app/` | Win32 layer: sockets, `WNet*`, credentials, tray, autostart | E2 |
| `windows/common/` | Service channel and discovery, already there (from the test program) | E1/E2 |

## Tests

- **E1:** state machine: all time limits (3 s, 5 s, 15 s, < 1 s) with a simulated clock; dropouts shorter than the disconnect time do not disconnect; wrong password without a storm.
- **E2:** real Windows client against the server in WSL as before. Server on: drive there. Server ended: drive gone. Server on again: drive back. A foreign mapping stays untouched.
  - **Open technical point:** in the CI the server does not run on port 445 but on 4450 (Windows itself occupies 445). `net use` can use another port (`/TCPPORT`), `WNetAddConnection2` cannot. The developer checks whether `WNetAddConnection4` can do this via its options. Otherwise the program gets a port from `HELLO` (`smb_port`) for the CI. That would also make sense for users with another port.
- **E3:** by the project owner: setup, 10 × PS5 off and on, times from the program's log, CPU when idle.

## Work packages

| Package | Content | Evidence |
|---|---|---|
| P3.1 | State machine with unit tests | E1 |
| P3.2 | Win32 layer: channel, heartbeat, create and remove `P:`, log. Still without tray, can be started from the command line | E2, E3 |
| P3.3 | Tray, notifications, installation dialog, autostart, uninstall via Windows | E2, E3 |
| P3.4 | Acceptance of phase 3: times and CPU, 10 cycles | E3, then release 0.1.0 |
