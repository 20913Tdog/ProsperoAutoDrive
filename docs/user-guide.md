# User guide

Prospero AutoDrive shows the file system of your PS5 as a drive in Windows Explorer, usually `P:`. The drive appears by itself when the PS5 is reachable and disappears when it is not. This guide covers release 0.1.0: setting it up, using it, removing it, and what to do when something does not work.

There are two parts:

- **`prospero-smbd.elf`** runs on the PS5. It is an SMB server, the same kind of file sharing Windows uses itself.
- **`ProsperoAutoDrive.exe`** runs on the PC. It is its own installer, sits in the notification area of the taskbar and connects the drive with the SMB client built into Windows.

## 1. What you need

- A PS5 with a jailbreak and a way to start ELF payloads from a file. This guide uses the Payload Manager (`pldmgr`); its pages are called "Manage Payloads", "Autoload", "Active Processes" and the dashboard.
- A PC with Windows 11 (Home, Pro or Enterprise). Windows 10 22H2 may work, but it is not tested regularly.
- PC and PS5 in the same home network. A guest network or "client isolation" in the router keeps them apart; then nothing can connect.
- TCP port 445 (SMB) from the PC to the PS5 must not be blocked, for example by a VPN or a security program. The program also uses port 4445 (TCP and UDP).
- Recommended: give the PS5 a fixed IP address in your router (a DHCP reservation). The program looks for the PS5 at its last address and does not search the network by itself (see [section 6](#6-troubleshooting)).

## 2. Set up the PS5

1. **Time zone and time.** In the PS5's settings under System, Date and Time, choose your time zone and then set the time by hand. The drive shows correct file times in any case: the PC sends its own time, and the server converts every file time it reports or sets by the measured difference. With a wrong time zone, though, the PS5's own clock is off by hours, and so are the times that programs on the PS5 itself show, for example an FTP server.
2. **Copy the file.** Put `prospero-smbd.elf` into the folder `/data/prospero-smbd/` on the PS5, for example with an FTP program. Create the folder if it does not exist. Do not use `/data/homebrew`.
3. **Register it.** In the Payload Manager, add `/data/prospero-smbd/prospero-smbd.elf` under "Manage Payloads".
4. **Start it automatically.** Add it to "Autoload", so that it starts every time the Payload Manager starts.
5. **Start it now** from the dashboard. The PS5 shows "Prospero AutoDrive running: share prospero on SMB port 445".

The server keeps its state in the same folder:

| File | Content |
|---|---|
| `config.ini` | server ID, name, user name, password, the names of the paired PCs, the measured clock difference |
| `log.txt`, `log.1.txt` | the server's log; when `log.txt` is full, it becomes `log.1.txt` |

The password is created on the first start and is never shown. Deleting `config.ini` resets the server: it gets a new identity and a new password, and every PC has to pair again. Delete only `config.ini`, not the whole folder, or the Payload Manager no longer finds the program.

To update the server, copy the new `prospero-smbd.elf` over the old one (this also works through the drive while the server runs) and start it again from the dashboard. A new start always ends a running server first.

## 3. Set up the PC

1. **Download** `ProsperoAutoDrive.exe` from the [release page](https://github.com/20913Tdog/ProsperoAutoDrive/releases/latest). Optionally, check that the file is complete. In a command prompt in the download folder, run

   ```
   certutil -hashfile ProsperoAutoDrive.exe SHA256
   ```

   and compare the result with the line for `ProsperoAutoDrive.exe` in `SHA256SUMS` from the same release page.
2. **Start it.** The program is not signed yet, so Windows SmartScreen may show "Windows protected your PC". Click "More info", then "Run anyway". On a PC with Smart App Control turned on, an unsigned program does not start at all; see [section 7](#7-known-limitations).
3. **Install.** The window "Install Prospero AutoDrive" proposes `C:\Program Files\ProsperoAutoDrive`. You can choose another folder with "Browse..."; a folder `ProsperoAutoDrive` is then created inside the one you choose, unless it has that name already. A protected folder such as `C:\Program Files` makes Windows ask once for administrator rights; a folder of your own (for example in your user folder) does not. "Start with Windows" and "Add to the Start menu" are ticked; untick them if you do not want them. Click "Install". The program itself always runs without administrator rights.
4. **Find the PS5.** The window "Set up Prospero AutoDrive" searches the network once. When it shows "Found: PS5 at …", click "OK". If nothing is found, check that the server runs, click "Search again", or type the PS5's IP address and click "OK".
5. **Pairing** happens by itself: the first PC that asks gets the user name and password from the server, and the program keeps them in the Windows Credential Manager. The PS5 shows "paired with PC …". Then the drive appears and Windows shows "PS5 connected as P:".

From now on the drive comes and goes with the PS5. You do not have to do anything.

**Another PC.** On a PC that is already paired, choose "Pair another PC" in the tray menu. For the next 10 minutes the server lets one more PC pair; set up the program on the other PC within that time.

**Pairing after each start.** After every start of the server, pairing is also open for 2 minutes, until one PC pairs. This is how a PC that lost its pairing, for example after removing and installing the program again, gets back in. Another device in your network could pair in that time as well; the PS5 shows a notification for every pairing.

**Update the program:** download the new `ProsperoAutoDrive.exe` and start it. The window says "Prospero AutoDrive is already installed. Update it in this folder"; click "Update".

## 4. The tray menu

Click the icon in the notification area (green: connected, grey: not connected). "PS5" stands for the name of the server; it is "PS5" unless it was changed in `config.ini`.

| Item | What it does |
|---|---|
| first line | The state, for example "PS5 connected as P:, wired" or "PS5 not reachable, last seen 2026-10-26 18:30". |
| **Open drive** | Opens the drive in Explorer. Only while connected. |
| **Search the network for PS5 (30 s)** | Searches the local network for the paired server, for example after the PS5 got a new IP address. |
| **Pair another PC** | Lets one more PC pair within the next 10 minutes. Only while connected. |
| **Pair with the new server...** | Appears only when a different server answers at the saved address, for example after `config.ini` was deleted on the PS5. After a confirmation, the old pairing is deleted and the program pairs with that server. Confirm only if it is your own PS5. |
| **Pair again** | Appears only after the server refused pairing. Tries again at once. |
| **Shut down PS5...** | Ends the server on the PS5 after a confirmation. The drive goes, and files open on it are closed. The server can only be started again on the PS5. Only while connected. |
| **Start with Windows** | Ticked: the program starts when you sign in to Windows. |
| **Show log** | Opens the program's log in Notepad. |
| **Exit** | Ends the program; the drive goes. |

If `P:` is already used by something else, it stays untouched; the program takes the next free letter backwards from `O:` and says so once.

## 5. Remove it

**On the PC:** Windows Settings, Apps, Installed apps, "Prospero AutoDrive", Uninstall. If it was installed into a protected folder, Windows asks for administrator rights. The removal

- ends the running program, which disconnects the drive,
- deletes the program file and its folder (the folder only if nothing else is in it), the Start menu entry and the entry under Installed apps,
- deletes, for the Windows user who removes it, the autostart, the stored user name and password in the Credential Manager, and the settings and log in `%LOCALAPPDATA%\ProsperoAutoDrive`.

The server still lists the PC as paired. That does no harm; if the PC is set up again later, see "Pairing after each start" in [section 3](#3-set-up-the-pc).

**On the PS5:** remove the entry from "Autoload" and "Manage Payloads" in the Payload Manager, stop the server, and delete the folder `/data/prospero-smbd/` with the program, `config.ini` and the logs.

## 6. Troubleshooting

**Where is the log?** On the PC: tray menu, "Show log" (`%LOCALAPPDATA%\ProsperoAutoDrive\log.txt`). On the PS5: `/data/prospero-smbd/log.txt`.

**The drive does not appear.**

- Does the server run? After its start, the PS5 shows "Prospero AutoDrive running". After a restart of the PS5, the drive only comes once the jailbreak and the Payload Manager have started the server.
- Has the PS5 got a new IP address? The program only tries the last known address. Choose "Search the network for PS5 (30 s)" in the tray menu. A fixed address in the router avoids this.
- Are PC and PS5 in the same network, without a guest network or client isolation in between? Does a VPN or security program block port 445?
- "Drive not connected: Windows error …" names the reason Windows gave; the log has the details.

**"PS5 found, but pairing is not possible (probably an old pairing)".** The server no longer lets this PC pair, usually because the program was removed and installed again. Restart the server from the dashboard; for 2 minutes after the start pairing is open, and the program tries again every 10 seconds by itself. "Pair again" in the tray menu tries at once.

**"A different server answers at …".** A server with another identity answers at the saved address. That is normal after `config.ini` was deleted on the PS5. Then choose "Pair with the new server..." in the tray menu. If you did not reset your server, check which device has that address.

**"PS5: the share refused the sign-in".** The stored password does not fit. The program tries again once a minute. If it stays, the log shows more.

**"P: is in use, the drive is O: instead".** Another drive or device uses `P:`. The program leaves it alone.

## 7. Known limitations

- **Not signed.** SmartScreen warns before the first start. With Smart App Control turned on, Windows does not start the program at all. Signing is planned.
- **The whole PS5 file system is shared, read and write**, without a protection list. Deleting or changing the wrong files can damage the console's data or keep it from starting. Be careful outside your own folders.
- **Pairing without a code.** The first PC that asks, or any PC within the times described in [section 3](#3-set-up-the-pc), gets the password. That risk in your own network is yours. The connection is signed against tampering, but not encrypted.
- **No automatic network search.** After the PS5 got a new IP address, use "Search the network for PS5 (30 s)".
- **Explorer does not refresh by itself** when files change on the PS5 or through another PC; press F5.
- **No file locks.** Two programs can write the same file at the same time without being stopped. Attributes such as "Read-only" and "Hidden" are not kept.
- **Copying within the drive** goes over the network twice, to the PC and back.
- **Speed:** on cable, writing reached 136 % and reading 101 % of the PS5's FTP server (ftpsrv) in our measurements. The final speed goal for 1.0.0 over Wi-Fi (gate G2) is not proven yet. During a large copy, opening a small file can take about a second.
- **Windows 10 22H2** is best effort.

More background: [decisions](decisions.md), [roadmap](roadmap.md).
