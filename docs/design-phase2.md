# Design phase 2: PS5 server `prospero-smbd.elf`

As of 2026-10-21. It is based on the spikes S1 to S7 and the decisions in [`decisions.md`](decisions.md). The project owner confirmed the proposals on 2026-10-21; only the share is called `prospero` instead of `PS5`.

## Goal

A payload that runs after the start without any further action:

- It shares the PS5's file system via SMB on port 445.
- The Windows program reaches it via the service channel on TCP 4445. Over it, the program pairs once and afterwards learns continuously that the PS5 is reachable. If the IP is unknown, it searches via UDP 4445.

The Windows program itself is phase 3. In phase 2, test scripts take over its role.

## What stays from the spike and what is new

| Area | Spike | Product |
|---|---|---|
| Share | new test folder | root `/`, read and write, without a protection list (D3) |
| SMB port | 445 (S1) | 445 |
| Port 4445 | HTTP control port for tests | **service channel** (TCP) |
| UDP discovery | port 4446 (S7) | port 4445, without time |
| Password | random, as a notification | random, transferred during pairing, never displayed |
| Clock | offset only measured | offset is compensated in all timestamps |
| Server ID | random per start | permanent, in the configuration |
| Run time | 15 to 90 minutes | unlimited |
| Profile | b | b (1 MiB socket buffers, TCP_NODELAY), until the measurement of the wait shows otherwise |

## Folder on the PS5

`/data/prospero-smbd/` belongs to the payload. It is created on the first start:

- `config.ini`: server ID, user, password, paired PCs, last measured clock offset
- `log.txt` and `log.1.txt`: ring log, at most 1 MiB each

The test rule does not concern this folder, because it is not a test folder. **Tests** still write only into newly created `_ProsperoAutoDrive_Test_*` folders, even though the share is now `/`.

## Service channel TCP 4445

Text lines in ASCII, one request and one reply per line. The program sets up the connection and keeps it open.

| Request of the PC | Reply of the PS5 |
|---|---|
| `HELLO 1` | `HELLO 1 server_id=<id> name=<name> share=<share> smb_port=445 pairing=<open/closed> nonce=<16 hex>` |
| `PAIR 1 pc=<name>` | `PAIRED 1 user=<user> password=<password>` or `PAIR-REFUSED 1 reason=<reason>` |
| `PING 1 seq=<n> utc_ms=<PC time> mac=<checksum>` | `PONG 1 utc_ms=<server time> skew_ms=<offset> trusted=<0/1>` |
| `OPEN-PAIRING 1 seq=<n> mac=<checksum>` | `PAIRING-OPEN 1 seconds=600` or `ERROR 1 reason=bad-mac` |
| `STATUS 1` | `STATUS 1`, then lines `key=value`, then `END 1` |
| `QUIT 1` (only from 127.0.0.1) | `BYE 1`, then the server ends |
| anything else | `ERROR 1 reason=<reason>` |

For `curl.exe` in the test scripts, the channel also understands an HTTP request: `GET /` returns the status, `GET /quit` ends the server (only from 127.0.0.1). This way product and test payloads end each other. The password is never in the status.

- **Heartbeat:** the program sends a `PING` every 5 s. If the answer stays away for 15 s and TCP 445 fails as well, the PS5 counts as gone.
- **Checksum (`mac=`):** at the end of the line. It is HMAC-SHA256 in lowercase hex; the key is the password, computed over `<nonce from HELLO> <line up to before " mac=">`. The nonce applies only to this connection, and the counter `seq` must grow on a connection. A counter instead of the time, because the PC clock can also be set back. This way a recorded line cannot be reused.
- **Clock:** the offset comes from the `utc_ms` of the `PING` lines. It is only adopted from a `PING` with a valid checksum. A `PING` without a checksum still gets an answer (`trusted=0`), as a pure heartbeat. So no other device can shift the times. It is rounded to whole seconds and only changed from a difference of 2 s. Until the first measurement, the value from `config.ini` applies.
- **Discovery:** this does not run over the service channel, but over UDP 4445 (see below).
- At most 8 connections at the same time. A connection without a line for 60 s is closed.

## Discovery over UDP 4445

In everyday use the program needs no discovery: it connects via TCP 4445 to the stored IP and checks the server ID. Only if that fails, for example during setup or after an IP change, does it send a broadcast into each of its own networks, as in S7 (passed at E3). If that does not help either, the user enters the IP. The program binds no fixed port, so no firewall window appears.

- Request: `PROSPERO-DISCOVER 1 nonce=<hex>`; further fields after it are ignored
- Reply: `PROSPERO-HERE 1 nonce=<hex> id=<server ID> service_port=4445 smb_port=445 name=<name>`
- The server ID shows whether it is the same PS5. The time no longer runs here, but via `PING` in the service channel.

## Pairing (variant C)

Decided: the first PC that reports gets user and password. The PS5 then reports "Prospero AutoDrive: paired with \<PC name\> (\<IP\>)". The details:

1. **First pairing:** open as long as no PC is paired yet. There is no time limit for it, because the user may set up the program only days later.
2. **Another PC:** in the program on an already paired PC, one chooses "Pair another PC". Pairing is then open for 10 minutes. An unknown PC cannot trigger this.
3. **Reset:** when there is no paired PC any more, for example after a reinstall. The user then deletes `config.ini` via a file manager or FTP. On the next start the payload is back in the state of the first start. This happens rarely.
4. **Password:** 24 random characters; that is enough so that the recorded NTLM handshake cannot be cracked offline. The user is `prospero`.

## Share

- **Name `prospero`** (project owner, 2026-10-21). The program gets the name via `HELLO`. It thus stays generic and does not know the name by heart.
- **Clock compensation in the backend:**
  - when reporting a time to Windows, subtract the offset
  - when setting a time, add the offset
  - new files without a given time keep the PS5's time. Windows sees it correctly through the compensation.

## Operation

- **One instance:** a newly started payload ends a running one. For this it sends `QUIT` over the service channel, which is only accepted from its own address 127.0.0.1.
- **Rest mode:** if accepting connections breaks off, the server opens the ports again. This is an outer loop that also survives a crash of a single connection.
- **Notifications:** only at start, on pairing and on errors
- **Autostart:** up to the user (PLDMGR, etaHEN). The payload needs nothing special for it.
- **Test payloads and product** both bind port 4445; only one runs at a time. For product tests, `tools\Run-Test.cmd` ends the product via `QUIT`. It fetches the password with `PAIR`, no longer via `/password`. Test writes stay limited to new test folders.

## Work packages

| Package | Content | Evidence |
|---|---|---|
| P2.1 | Skeleton: payload `prospero-smbd`, folder, `config.ini`, ring log, one instance, share `/`, service channel with `HELLO`, `STATUS`, `QUIT` | E1, E2 |
| P2.2 | Discovery (UDP 4445), pairing and heartbeat in the service channel, test script in the role of the program | E1, E2, E3 |
| P2.3 | Clock compensation in the backend | E1, E2, E3 |
| P2.4 | Wait under load: measure first (time spent in the server, fill level of the send queue), then fix | E3 with `S4-Check.cmd` |
| P2.5 | Semantics: share modes, locks, delete-on-close, file information, security query, directory class 0x51. The yardstick is smbtorture. | E1, E2 |
| P2.6 | fsx, differential tester against NTFS, 7-Zip, fuzzing | E2 |

A live test (E3) is needed after P2.2, P2.3 and P2.4; otherwise the CI is enough.
