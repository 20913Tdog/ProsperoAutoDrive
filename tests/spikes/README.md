# Feasibility spikes

Before any product code, spikes S1–S7 establish whether the "native SMB" architecture holds up (gate G1). Each spike states its evidence level:

- **E1:** CI on Linux
- **E2:** CI with a real Windows client
- **E3:** real PS5

Program output quoted below (status lines, script results, folder and file names of the test data) is reproduced exactly as it was at the time of the run, so some of it is in German.

## One-click test

Start `tools\Run-Test.cmd`, by double-clicking it or from a console. The first time, it asks for the PS5's IP address and stores it in `tools\ps5.local.ini` (not in git). After that everything runs without questions:

1. `git pull` on `dev`
2. fetch the verified build. It must match the checked-out state exactly; otherwise the script stops, because the CI is still running.
3. close all connections to the PS5 and end a running test payload
4. send the payload (ELF loader, port 9021) and wait until it is ready
5. fetch the password once over port 4445, run the current test and end the payload afterwards

At the end, the complete output is in the clipboard and in `logs\<test>.log`. It only needs to be pasted into the chat. Close Explorer windows pointing at the PS5 beforehand.

Which test runs is set by `DEFAULT_TEST` in the script. Another test can be given directly: `tools\Run-Test.cmd S2 [PS5-IP]`. The individual steps in the sections below remain as the manual route.

## S1 – Can a payload bind TCP 445?

**Question:** May a payload on the PS5 open the SMB port 445 (and the fallback port 4445), and is 445 not already taken by something else?

**What the probe does:**

- The payload `prospero-s1-portprobe.elf` tries to bind 445 and 4445 and shows the result as a PS5 notification.
- On every port it bound, it answers simple HTTP requests with the result for both ports and the reason for any failure.
- It writes **no files** and ends by itself after 10 minutes.

**Already verified (E1, CI on Linux):** the same source code with free ports, with 445 taken by another service, and on a restart over a running instance.

### Procedure on your PC

1. Update to `dev` and fetch the verified build:
   ```
   git pull
   tools\Get-DevBuild.cmd
   ```
   Every line must show `OK`. The payload is then in `dist\dev\`.
2. **Before measurement** (payload not started yet):
   ```
   tests\spikes\S1-Check.cmd <PS5-IP>
   ```
   Expected on both ports: nothing listening. If something already answers on 445, that is a result in itself: 445 is taken.
3. **Start the payload:** the way you normally load payloads, or with the included helper to the ELF loader (port 9021):
   ```
   tools\Send-Payload.cmd <PS5-IP> dist\dev\prospero-s1-portprobe.elf
   ```
   The PS5 shows `Prospero S1: 445 OK, 4445 OK (uid 0)` or an error message.
4. **After measurement:**
   ```
   tests\spikes\S1-Check.cmd <PS5-IP>
   ```
5. Report the complete output of both measurements and the text of the PS5 notification.

### Evaluation

| Result | Meaning | Consequence |
|---|---|---|
| Before: nothing listening on 445; after: `bind 445: OK` on both ports | **S1 passed** | primary path port 445 |
| `bind 445: FAIL … Address already in use` | another service on 445 | fallback decision: alternative port 4445 (Windows 11 24H2+ only) or identify the other service |
| `bind 445: FAIL … Permission denied` | no rights for ports < 1024 | raise privileges in the payload, test again |
| 4445 reachable, 445 times out | port 445 is filtered on the way (e.g. by the router or a Wi-Fi device) | check the network, evaluate the alternative port |
| both ports time out | PS5 not reachable or payload not started | check the IP and the payload start |

### Result: passed (E3, 2026-10-20, build `95dabbe`)

After measurement from the Windows PC to the PS5 over Wi-Fi, identical on both ports except for `connected_port`:

```
PROSPERO-S1 OK
connected_port=445
bind 445: OK
bind 4445: OK
uid=1 euid=0
```

**Assessment:**
- The payload binds 445 with effective root rights (`euid=0`), and Windows reaches 445 over Wi-Fi, so nothing filters it on the way.
- Another service on 445 is ruled out. Even a service bound only to the PS5's specific IP would have received the connection before our wildcard binding.
- **Consequence:** port 445 is the primary path; the alternative port is dropped. Noted for S3: check which owner and which permissions newly created files get (`uid=1`, `euid=0`).

## S2 – Does the libsmb2 server engine serve the Windows client correctly?

**Question:** Does the libsmb2 server engine with our own file system backend deliver a folder correctly and completely to the built-in SMB client of Windows 11, with NTLMv2 and mandatory signing? S2 checks reading only; writing follows in S3.

**What the payload does:**

- `prospero-s2-smbserver.elf` creates a **new** test folder `/data/_ProsperoAutoDrive_Test_<time>-<random>` and fills it with fixed test files, about 65 MB in total (list in `ps5/spikes/smbserver/testdata.c`).
- Exactly this folder is shared **read-only** as `\<PS5-IP>\s2`. User `prospero`; the password appears as a PS5 notification.
- Port 4445 shows the status, including the requests the backend does not support yet (`unsupported:`) and why the last connections ended (`connection_end:`).
- It ends by itself after 15 minutes, earlier with `curl.exe http://<PS5-IP>:4445/quit`.
- Outside the new test folder it reads and writes nothing. The test folder stays on the PS5 afterwards. It is not deleted automatically; whether and when it goes is your decision.

**Already verified (E1, CI on Linux, Samba `smbclient` with mandatory signing):** complete recursive download with SHA-256 comparison, the dialects SMB 2.0.2 to 3.1.1, a folder listing with 1500 entries, case handling, refusal of create, delete and rename, no paths out of the share, wrong password, unknown share, file and volume information.

### Procedure on your PC

1. Update to `dev` and fetch the verified build:
   ```
   git pull
   tools\Get-DevBuild.cmd
   ```
2. Start the payload:
   ```
   tools\Send-Payload.cmd <PS5-IP> dist\dev\prospero-s2-smbserver.elf
   ```
   The PS5 shows `Prospero S2 bereit: Freigabe s2 (nur lesen), Benutzer prospero, Passwort xxxx-xxxx-xxxx (15 min)`.
3. Start the check script and enter the password when it asks:
   ```
   tests\spikes\S2-Check.cmd <PS5-IP>
   ```
4. At step 7 the script waits. Now enter `\<PS5-IP>\s2` in the Explorer address bar and:
   - open the folder `viele` and scroll to the end (1500 files),
   - open `Ordner mit Leerzeichen`: is `Grüße äöü €.txt` shown correctly?
   - open `hello.txt` by double-clicking it,
   - copy `pattern-64MiB.bin` to the desktop, note the rough duration and delete the copy afterwards,
   - right-click `tief` → Properties,
   - drag any file from the desktop into the folder: this must fail.

   Then press a key in the script window.
5. Report the **complete script output** and anything odd in Explorer (error messages, waiting times, wrong display).

### Evaluation

| Result | Meaning | Consequence |
|---|---|---|
| `S2-CHECK: ALL OK`, Explorer unremarkable | **S2 passed** | continue with S3 (writing) |
| step 1: system error 1326 | logon refused | check the password. If it is right: NTLMv2 problem, analyse |
| step 1: system error 53 or 67 | server or share not found | is the payload running? check the IP |
| step 1: another error | e.g. a signing or dialect problem | analyse the error text, blocker for S2 |
| `FAIL` or `MISSING` in the checksums | data arrives wrong | blocker |
| `unsupported:` lines in the status | Windows asks for something the backend cannot do | assess one by one |
| very high counter for `CHANGE_NOTIFY` | Explorer repeats change notification in a loop | add pending notifications in S3 |

### Known limits of this prototype

- Change notifications (`CHANGE_NOTIFY`) are refused; Explorer then refreshes the view only with F5.
- After a mistyped password, a second attempt on the same connection may fail (libsmb2, SMB 3.1.1). Then `net use \<PS5-IP>\s2 /delete` and connect again.
- libsmb2 sends the replies to compound requests one by one instead of bundled. Only this test shows whether Windows accepts that without drawbacks.

### Result: functionally passed (E3, 2026-10-20, build `dc283f6`)

Windows 11 Pro 25H2 (build 26200) over Wi-Fi, `S2-CHECK: ALLES OK`, Explorer test unremarkable.

**Assessment:**
- Logon with NTLMv2 works. Windows 11 Pro requires signing, so the session was signed. The missing SPNEGO mechListMIC does not bother Windows, unlike Samba's `smbclient`.
- All 1507 files arrived with the correct SHA-256. Case handling, Unicode names and the folder with 1500 entries work, and all write attempts were refused.
- The status shows `lost_replies=0` and no aborted connections.

**What Windows additionally asks for (`unsupported:`):**

| Request | Meaning | Assessment |
|---|---|---|
| `QUERY_DIRECTORY class 0x51` ×16 | FileIdAllExtdBothDirectoryInformation (Windows 11) | Windows falls back cleanly to 0x25. Adding it saves one request per folder |
| `QUERY_INFO type 3` ×1508 | security descriptor, robocopy asks per file | not critical. In the product, return a simple ACL (Explorer "Security" tab) |
| `CREATE on IPC$ srvsvc` ×74, `wkssvc` ×6 | share and workstation information over RPC | not critical for a drive. Check for the product, as 74 failed attempts cost waiting time |
| `CHANGE_NOTIFY` ×12 | Explorer's change notification | **no loop**: Windows accepts the refusal. Live refresh later |
| `IOCTL 0x000900C0` ×12 | FSCTL_CREATE_OR_GET_OBJECT_ID (link tracking) | not critical |
| `IOCTL 0x00094264` ×2 | FSCTL_OFFLOAD_READ (ODX copy offload) | not critical, Windows then copies normally |
| `IOCTL 0x00110018` ×1 | FSCTL_PIPE_WAIT on IPC$ | not critical |

**Speed: far too slow.**
- robocopy needed 5:38 min for all 65 MB (≈ 0.2 MB/s). Without the 64 MiB file that is ≈ 180 ms per small file.
- Explorer needed ≈ 65 s for 64 MiB (≈ 1 MB/s, fluctuating strongly).
- Gate G1 cannot be reached like this. That is why the root-cause analysis S2b comes before S3.

**Noted for S3:** The PS5's file times did not match robocopy's end time. The measurement in S2b shows why: internally the PS5 runs 1 hour ahead (see there). This matters for timestamps when writing.

## S2b – Why is reading so slow?

**Question:** Is the speed in S2 caused by socket buffers on the PS5 that are too small, by the SMB3 signature (AES-CMAC, in libsmb2 without hardware acceleration) or by the Wi-Fi itself?

**Background:**
- ftpsrv sets 1 MiB send and receive buffers for every data connection. That points to small default buffers on the PS5.
- libsmb2 computes AES-CMAC with a simple software AES and expands the key anew for every 16-byte block. That costs a lot of CPU time per MiB.

**What the new payload can do in addition:** over port 4445 it can be switched between three profiles. It counts CPU time (`cpu_user_ms`, `cpu_sys_ms`), read time from the SSD (`pread_ms`) and the time window of all READs (`read_window_ms`).

| Profile | Socket | SMB version and signature |
|---|---|---|
| a | PS5 default, exactly as in S2 | 3.1.1, AES-CMAC |
| b | 1 MiB buffers, TCP_NODELAY (as ftpsrv) | 3.1.1, AES-CMAC |
| c | as b | 2.1, HMAC-SHA256 |

**Already verified (E1, CI):** switching all profiles, the negotiated dialect (3.1.1 or 2.1) and the counters.

### Procedure on your PC

Easiest with `tools\Run-Test.cmd` (see above), as S2b is the current test. By hand:

1. Update to `dev` and fetch the build:
   ```
   git pull
   tools\Get-DevBuild.cmd
   ```
2. Start the payload **anew**, so that profile a is unchanged:
   ```
   tools\Send-Payload.cmd <PS5-IP> dist\dev\prospero-s2-smbserver.elf
   ```
3. Close all Explorer windows pointing at the PS5. Then start the measurement script and enter the password from the PS5 notification:
   ```
   tests\spikes\S2-Speed.cmd <PS5-IP> [FTP-Port]
   ```
   The FTP port is optional (default 2121, ftpsrv). It is for the FTP server that already runs on the PS5 and provides the Wi-Fi reference. The script first closes all existing connections to the PS5. Windows allows only one user name per server (system error 1219).

   This takes a few minutes. During that time, avoid network load on the PC and the PS5 as far as possible: no downloads and no streaming.
4. Report the complete output up to `S2-SPEED: FERTIG`.

### Evaluation

| Observation | Meaning | Consequence |
|---|---|---|
| b clearly faster than a | the socket buffers slow it down | adopt profile b |
| c clearly faster than b, `cpu_user_ms` close to `read_window_ms` | the signature computes too slowly | build a fast AES-CMAC (AES-NI) into libsmb2, as a contribution upstream |
| a, b and c about as slow as the FTP reference | the Wi-Fi is the limit | the comparison with ftpsrv in S4 decides |
| all clearly slower than the FTP reference, CPU low | waiting times in the protocol flow | examine the flow more closely |

### Result (E3, 2026-10-20, build `5c2412e`, Wi-Fi, 2 runs per profile)

| Measurement | FTP (ftpsrv) | a | b | c |
|---|---|---|---|---|
| 64 MiB, run 1 | 5.2 MB/s (before) | 2.7 MB/s | 2.1 MB/s | 3.6 MB/s |
| 64 MiB, run 2 | 3.4 MB/s (after) | 0.8 MB/s | 3.8 MB/s | 2.9 MB/s |
| 100 small files | – | 15 s | 6 s | 8 s |
| CPU per 64 MiB (`cpu_user_ms`) | – | ≈ 1.95 s | ≈ 1.9 s | ≈ 0.58 s |
| SSD read time per 64 MiB (`pread_ms`) | – | ≈ 50 ms | ≈ 50 ms | ≈ 60 ms |

**Assessment:**
- **Small files:** b is 2.5 times faster than a, about 60 instead of 150 ms per file. That fits TCP_NODELAY: without this option, the Nagle algorithm and delayed acknowledgements hold back every small reply. **Profile b is adopted.**
- **Signature:** AES-CMAC costs a good three times as much CPU time as HMAC-SHA256, about 30 ms per MiB. That caps at around 33 MB/s, so over Wi-Fi it does not slow things down yet. For G2 and wired networks a fast AES-CMAC remains necessary. SMB 3.1.1 stays.
- **Wi-Fi:** The SSD does not slow things down. The Wi-Fi fluctuates strongly: even the FTP reference fell from 5.2 to 3.4 MB/s. With two runs per profile the gap to ftpsrv cannot be stated reliably. The SMB runs with b and c were at 2.1 to 3.8 MB/s, roughly 50 to 90 % of the FTP values. Only S4 settles this, with ≥ 7 alternating runs and the median.
- **Clock:** The PS5's clock (`now_utc`) runs exactly **1 hour ahead** of real UTC (github.com); the PC and the PS5's display are right. Presumably the PS5 applies daylight saving time only to the display. Important for S3, because timestamps the PS5 sets itself would be 1 hour off during daylight saving time.

## S3 – Does Windows write correctly to the PS5?

**Question:** Can the Windows client create, write, overwrite, rename, move and delete files and folders on the PS5 through our backend? Do all bytes and modification times arrive correctly? Who owns newly created files, and which time do they get?

**What the payload does:**

- `prospero-s3-smbserver.elf` creates, as in S2, a **new** test folder `/data/_ProsperoAutoDrive_Test_<time>-<random>` with the same test files. It shares this folder as `\<PS5-IP>\s3`, **for reading and writing**.
- Writes happen only inside this folder, because the server refuses names that lead out of the share. The folder stays afterwards. Whether and when it goes is your decision.
- Socket settings as profile b from S2b. The payload runs for 30 minutes.
- Not in this prototype:
  - Share modes and byte-range locks are accepted but not enforced.
  - Attributes other than directory and archive, and the creation time, are not stored, only counted.
  - There are no alternate data streams.

**Already verified (E1, CI on Linux, Samba `smbclient`, SMB 3.1.1 with signing):**

- uploading all 1507 test files with SHA-256 comparison on the server
- overwriting truncates the file; the existing spelling is kept
- creating folders, including refusing a folder that differs only in case
- non-empty folders are not deleted; a folder tree can be deleted
- renaming, including a change of case only, moving, replacing only on request, renaming folders
- deleting
- modification time set by the client
- no writes outside the share, no invalid names
- owner of new files

### Procedure on your PC

Start `tools\Run-Test.cmd` (see above), as S3 is the current test. Close Explorer windows pointing at the PS5 beforehand. The run takes about 10 minutes.

What `tests\spikes\S3-Check.cmd` checks:

1. **Fetch the template:** the test files are copied to the PC.
2. **Upload:** robocopy puts everything into the new folder `hochgeladen`.
3. **Sizes and times:** size and modification time of every file must match. A robocopy dry run checks this.
4. **Byte by byte:** everything is downloaded again and compared with the checksums.
5. **Editing in the folder `ops`:** create, append, overwrite, copy within the share, rename, change only the case, move, rename and delete a folder. A non-empty folder must not be deleted. A small folder tree the test just created itself is deleted recursively.
6. **Time:** a new file gets its time from the PS5's clock. The script shows it next to the PC time and next to UTC from github.com.
7. **Speed:** 64 MiB are written into the test folder via SMB and, for comparison, via FTP (ftpsrv, port 2121).

### Evaluation

| Result | Meaning | Consequence |
|---|---|---|
| `S3-CHECK: ALL OK` | **S3 functionally passed** | assess timestamps and speed, continue with S4 |
| error while uploading or `FAIL` for bytes, sizes or times | data or times arrive wrong | blocker, analyse |
| single `FAIL` in step 5 | a file operation behaves differently than on Windows | assess one by one |
| time of the new file 1 hour off | the PS5's clock shows through, see S2b | design a correction in the server |
| `unsupported:` or `delete_failures` lines in the status | Windows asks for something the backend cannot do | assess one by one |

### First run (E3, 2026-10-20, build `1ec44bd`, Wi-Fi)

Build `20de186` stayed silent after starting: the PS5 showed "ready", but port 4445 did not answer. The likely cause was `localtime_r()`, the only new library function on this path. It has been removed since `1ec44bd`, and the payload answers. That does not prove the suspicion, but it fits.

**Works:**
- All 1507 files arrived byte for byte identical, uploaded and downloaded again.
- All 13 file operations in `ops` behaved as on Windows, including the change of case only, moving, the refusal to delete a non-empty folder and the recursive delete.
- Copying within the share works: after the refused server-side copy (`IOCTL 0x00140078`), Windows falls back to reading and writing.
- **Write speed, 64 MiB, one run:** SMB 6.9 MB/s, FTP (ftpsrv) 6.8 MB/s.

**Failed: modification times.** Not a single one of the 1507 times was applied (`times_set=0`).
- **Cause:** The SDK provides `utimensat()` only as a bare FreeBSD system call. The PS5's libkernel does not have this function, on the console the number may mean something else, and the result does not end up in `errno`. That is why the status showed a stale `errno 35`.
- **Fixed:** From the next build on, the backend uses `utimes()` from libkernel. Times are then accurate to the microsecond, while Windows works in 100 ns. That is why the check compares with a 2-second tolerance (`/FFT`) and additionally shows the exact deviation.
- **Safeguard:** The CI now fails as soon as our code or libsmb2 uses such a bare system call (`.github/scripts/check-raw-syscalls.sh`).

**Time confirmed:** A new file written without a time gets the PS5's time. In Explorer it is exactly **1 hour in the future**: robocopy shows 18:47:01 UTC, github.com 17:47:00 UTC. The server must correct this. For that, `GET /clock` asks what the PS5 knows about its time zone.

**Further findings:**
- New files are owned by `uid=0 gid=0` and have the permissions `0666`. So there is no umask; everyone may read and write.
- New in `unsupported:` is `IOCTL 0x00140078`, the server-side copy. The security query (`QUERY_INFO type 3`) came 3015 times; robocopy asks for it per file. Both are to be added for the product.

### Result: passed (E3, 2026-10-20, build `46a55e4`, Wi-Fi)

`S3-CHECK: ALLES OK`.

- Bytes, all 13 file operations and the copy within the share are right, as in the first run.
- **Modification times:** all 1507 are right, even without tolerance (`times_set=3030`, two calls per file). The PS5's times only have microsecond resolution anyway, so `utimes()` loses nothing here.
- **Write speed, 64 MiB, one run:** SMB 7.7 MB/s, FTP 7.2 MB/s. Uploading 1507 files with 65 MB took 1:40 min, about 66 ms per file.
- **PS5 clock:** New files are again exactly 1 hour in the future. According to `GET /clock` the kernel reports `tz_minuteswest=0` and `tz_dsttime=60`, and `sceKernelGettimezone` returns the same values. So the PS5 keeps its kernel time as "UTC" and knows about 60 minutes of daylight saving time. Its clock runs ahead by exactly these 60 minutes.
  - **Assumption:** daylight saving time is part of the kernel clock itself. Then the deviation disappears after daylight saving time ends on 2026-10-25. This can be checked with a run from 2026-10-26 on.
  - **Certain:** the deviation is constant and can be measured.
- **For the product** the server must correct the deviation in both directions: subtract it when it reports times, and add it when a client sets times. That way the times on disk stay in the PS5's time reference, matching ftpsrv and other programs, and Windows sees real times. Where the server learns the deviation from is still to be decided.

## S4 – Is the throughput sufficient?

**Question:** Does SMB over Wi-Fi reach at least 70 % of ftpsrv's throughput in both directions (gate G1)? What does the signature cost, and how long does a small request wait while a large copy is running?

**What the payload does:** `prospero-s4-smbserver.elf` creates, as in S3, a **new** test folder and shares it as `\<PS5-IP>\s4` for reading and writing. In addition there is the file `pattern-256MiB.bin`. Profile b as in S3. The payload runs for at most 90 minutes.

**Already verified (E1, CI on Linux, Samba `smbclient`):** 256 MiB file with the correct checksum, reading and overwriting twice via SMB, switching to profile c with SMB 2.1, counters for CPU and bytes. Nothing is measured there.

### Procedure on your PC

Start `tools\Run-Test.cmd`, as S4 is the current test. Close Explorer windows pointing at the PS5 beforehand. ftpsrv must be running on port 2121. The run takes about 30 minutes, up to an hour with weak Wi-Fi. The script estimates the duration after the first copy. During that time, avoid network load on the PC and the PS5 as far as possible.

What `tests\spikes\S4-Check.cmd` measures:

1. **Preparation:** checks that ftpsrv answers, fetches the file once via SMB and checks its SHA-256.
2. **Reading a small file:** `hello.txt` 20 times, once without load and once during a large copy.
3. **7 rounds with profile b** (SMB 3.1.1, AES-CMAC): per round SMB read, FTP read, SMB write and FTP write. The order alternates every round, so that Wi-Fi fluctuations hit both equally. In round 1 the files read are also compared byte by byte.
4. **3 rounds with profile c** (SMB 2.1, HMAC-SHA256), SMB only.
5. **Evaluation:** median per direction, SMB as a percentage of FTP, the PS5's CPU time per MiB.

Only `schreiben-smb.bin` and `schreiben-ftp.bin` in the new test folder are written, overwritten anew every round. If a transfer fails, the measurement ends without a retry.

### Evaluation

| Result | Meaning | Consequence |
|---|---|---|
| `S4-CHECK: G1 PASSED` | median SMB ≥ 70 % of FTP in both directions | S4 passed, continue with S5 |
| `G1 NOT REACHED`, profile c clearly faster | the signature slows it down | fast AES-CMAC, then measure again |
| `G1 NOT REACHED`, profile c similar, CPU low | waiting times in the protocol or in the server | examine the flow, e.g. more parallel requests |
| small file very slow during the copy (seconds) | small replies wait behind the large ones (risk R4) | in phase 2 limit credits, worker threads if needed |
| `S4-CHECK: FEHLER` | a transfer failed | analyse the output, retry nothing automatically |

### Result: G1 passed (E3, 2026-10-20, build `299cb3d`, Wi-Fi)

`S4-CHECK: G1 BESTANDEN`. 7 alternating rounds per direction with 256 MiB. Compared byte by byte were the first copy (SHA-256) and the two files read in round 1. For all other runs, including all write runs, the script checks only the size.

| Direction | SMB 3.1.1 (median) | ftpsrv (median) | ratio of the medians | median of the per-round ratios | range of the ratios |
|---|---|---|---|---|---|
| read | 3.90 MB/s | 4.06 MB/s | **96 %** | 100 % | 70–119 % |
| write | 7.10 MB/s | 7.20 MB/s | **98 %** | 99 % | 96–101 % |

The per-round ratio compares SMB with the FTP run right next to it, which removes the Wi-Fi drift. The script evaluates only the ratio of the medians. The two right-hand columns were recalculated by hand from the output.

**Assessment:**
- **G1 (≥ 70 %) is clearly reached in both directions.**
- **Writing:** SMB is level with FTP, between 96 and 101 % in every round. Both drop together in rounds 6 and 7; there the Wi-Fi is the limit.
- **Reading:** level in the median as well, but the single rounds scatter strongly, from 70 to 119 %. With 7 rounds the read ratio is only known to about ±8 %. Twice SMB reading dropped to about 2.7 MB/s (round 7, profile c round 2); FTP reading never went below 3.38 MB/s. With so few runs this is not conclusive, but worth keeping an eye on.
- **Reading is only a good half as fast as writing with both protocols.** So it is not the protocol. What else causes it is open, for example the radio link, the router or the PC.
- **Signature:** SMB 2.1 with HMAC-SHA256 was not faster (read 4.00, write 6.56 MB/s, 3 runs each). This comparison is not conclusive: the 3 runs came at the end, without an FTP run next to them, and the Wi-Fi was already weaker by then. The stronger argument: SMB 3.1.1 reaches almost the FTP values anyway, so the signature can hardly slow things down over Wi-Fi. **SMB 3.1.1 stays.**
- **PS5 CPU (whole process, getrusage):** 36 to 39 ms per MiB with SMB 3.1.1, 16 to 18 ms with SMB 2.1. **So AES-CMAC costs about 20 ms per MiB more than HMAC-SHA256.**
  - The server runs in a single thread. At 36 to 39 ms per MiB it reaches at most about 26 to 27 MiB/s, with HMAC about 56 MiB/s.
  - Over Wi-Fi the process was 14 % (reading) and 26 % (writing) busy. The extra cost of AES-CMAC accounts for about 7 and 14 percentage points of that.
  - It only matters on a wired network.
- **Waiting time under load: 2925 ms** per small file during a large copy, compared with 44 ms without load. It is one measurement with 20 reads. It does not count for G1, but it does for how Explorer feels: a folder probably opens only after seconds during a copy.
  - For each small file Windows sends several requests (open, read, close). So each request waits roughly 0.7 to 1 s.
  - **Proven (libsmb2 code):** the server grants every credit the client asks for (`lib/pdu.c`). It keeps reading further requests without throttling (`smb2_which_events` in `lib/socket.c`), and appends every reply to the end of the send queue (`SMB2_LIST_ADD_END`). So a small reply waits behind all large read replies already in the queue.
  - **Not proven:** how much of the waiting time comes from this queue. Further buffers sit in the same line: the 1 MiB send buffer of profile b, about 0.25 s at 4 MB/s, plus the buffers in the Wi-Fi and the router. How many reads Windows keeps open at the same time has not been measured either.
  - **Next step:** measure first, then fix. The server is to report how long a request sits with it and how full the send queue is. Compared are a 1 MiB send buffer against the default, and unlimited against limited credits. Only then is it decided whether to limit credits, prefer small replies or build worker threads. Worker threads alone do not resolve a jam in the send queue.
- **Clock:** still 1 hour ahead (kernel UTC 22:35:56, github.com 21:35:59). The check from 2026-10-26 on is still pending.
- As in S3, the status shows only known `unsupported:` lines (directory class 0x51, security query, offload read). The `connection_end` comes from switching to profile c.

## S6 – Can the server be tested with real clients in the CI?

**Question:** Can the real SMB client of Windows and the Linux kernel client run against our real server code in the CI, without MSVC and without a PS5?

**Setup (jobs in `.github/workflows/ci.yml`):**

- The CI additionally builds the server statically for Linux (`host-servers`). It is the same code as on the PS5, only with Linux as the platform instead of the PS5.
- **`windows-client` (windows-2025):**
  - The server runs in WSL 1, in a minimal Alpine rootfs pinned by checksum. WSL 1 shares the network with Windows.
  - Windows itself occupies port 445, so the server listens on 4450. The client connects with `net use ... /TCPPORT:4450`.
  - Then **the same `S3-Check.cmd` as on the PS5** runs (environment variable `PROSPERO_SMB_PORT`).
- **`linux-cifs` (ubuntu-24.04):**
  - Kernel mount with `mount -t cifs`, SMB 3.1.1 with signing. All checksums are read, one file is written.
  - Plus a selection from `smbtorture`, compared with [`smbtorture-pass.txt`](smbtorture-pass.txt): if a subtest from this list fails, the CI fails. The job reports newly passing ones; they are added by hand.
- `dev-build` is only published when both jobs are green.

### Result: passed (E2, 2026-10-21, builds `b26d89c` and `e3e1b99`)

- **Windows client:** `S3-CHECK: ALLES OK` with Windows Server 2025, signing mandatory, guest access off. Bytes, sizes, times and all 13 file operations are right. The `unsupported:` lines are the same as live on the PS5, except for the counts (directory class 0x51, security query 3015 times, the two IOCTLs).
- **Linux cifs:** mounting, reading with checksums and writing work.
- **smbtorture:** 13 of 43 subtests pass and are now on the list. The remaining 30 show what is missing in phase 2, above all share modes and delete-on-close (`smb2.delete-on-close-perms`, `smb2.rename`), file information (`smb2.getinfo`, `smb2.setinfo`) and `smb2.connect`.

**What the CI does not cover (E3 remains for that):**

- port 445: in the CI everything goes through the alternative port.
- the PS5's file system: in the CI it is that of WSL 1 or Linux. For example, new files there have the permissions `0644`, on the PS5 `0666`.
- the PS5 build itself, rest mode, Wi-Fi, throughput

**Small difference:** On the PS5 all times matched exactly; in the CI about 1350 of 1507 differ without tolerance. The test files are created there with nanoseconds, while `utimes()` sets only microseconds. With a 2 s tolerance all of them match.

## S7 – Does a Windows program find the PS5 without a firewall dialog?

**Question:** Does a program running as a normal user find the PS5 over Wi-Fi via UDP, directly (unicast) and by broadcast, without Windows showing a firewall window? And can the server measure its clock deviation from the PC time at the same time (variant 1)?

**What the parts do:**

- `prospero-s7-smbserver.elf` creates, as in S2, a **new** test folder and shares it as `\<PS5-IP>\s7` **read-only**. In addition it answers UDP requests on port 4446 (`ps5/spikes/smbserver/discovery.c`). It runs for 30 minutes.
- `prospero-s7-discover.exe` is the first Windows program, C++ with llvm-mingw, statically linked. It sends the request with the PC's UTC time, first to the PS5's IP, then as a broadcast into every network of the PC. It binds no fixed port but, like any client, gets a random one.
- **Protocol** (one text line per packet):
  - Request: `PROSPERO-DISCOVER 1 nonce=<hex> utc_ms=<PC time>`
  - Reply: `PROSPERO-HERE 1 nonce=<hex> id=<server ID> smb_port=<port> utc_ms=<server time> skew_ms=<deviation> name=<name>`
  - Anything else gets no answer.

**Already verified:**
- **E1 (Linux):** valid and invalid requests, measured deviation, counters, share running alongside
- **E2 (windows-2025):** unicast over loopback against the server in WSL 1. Whether a firewall window appears is not shown by the CI, because the runner runs with admin rights and without a screen. Only your run checks that.

### Procedure on your PC

Start `tools\Run-Test.cmd`, as S7 is the current test. Please do **not** open the console as administrator. The run takes about a minute. If a Windows Firewall window appears, click **Cancel**. At the end the script asks whether such a window appeared: enter `j` (yes) or `n` (no).

### Evaluation

| Result | Meaning | Consequence |
|---|---|---|
| `S7-CHECK: PASSED` | unicast and broadcast arrive, no window | S7 passed, decide G1 |
| unicast OK, broadcast no answer | the broadcast does not get through the Wi-Fi or the router | the program finds the PS5 through the saved IP; search by broadcast only as an aid during setup |
| firewall window appeared | Windows sees the program as a server | change the approach, e.g. unicast only or TCP |
| no answer, `ICMP reports the port unreachable` | the payload does not listen on UDP 4446 | check the payload status |
| `discovery_skew_ms` about 3600000 | PS5 clock 1 hour ahead, measured from the PC time | variant 1 works |

### Result: passed (E3, 2026-10-21, build `ff9d50a`, Wi-Fi, Windows 11 25H2)

`S7-CHECK: BESTANDEN`. Console without administrator rights, active firewall profile **Public**, i.e. the strictest, with notifications switched on.

- **Unicast:** 3 of 3 requests answered, after 4 to 5 ms.
- **Broadcast:** In each of the 3 attempts 2 replies came, after 26 to 100 ms. Both the broadcast into the local network (the subnet's broadcast address) and the general one (255.255.255.255) reached the PS5. As expected, the network of the WSL interface got no answer. The payload counted 9 requests and 9 replies, none invalid.
- **No firewall window.** Windows lets the replies to its own requests through, even to a broadcast, without the program binding a fixed port.
- **Clock (variant 1):** The PS5 runs 3596.5 s ahead of this PC's clock, i.e. 1 hour minus 3.5 s. Across the 9 measurements the value scattered by less than 0.1 s. Compared with github.com the PS5 still runs about 3599 s ahead. So the PC clock itself deviates by a few seconds. For the drive this is right: the times are meant to match the clock of the PC that Windows works with.
