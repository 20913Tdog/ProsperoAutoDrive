# Plan: completing release 0.1.0

As of 2026-10-26. This document describes everything that is still missing for the first release 0.1.0, so that a new working session can take it over without prior knowledge. It complements [`CLAUDE.md`](../CLAUDE.md) (binding rules), [`roadmap.md`](roadmap.md) (phases and results) and [`decisions.md`](decisions.md) (reasons).

## 0. Starting point

### What is done (with evidence level)

| Area | Status |
|---|---|
| PS5 server `prospero-smbd.elf`: SMB, service channel, discovery, pairing, clock skew, many files | E1 + E2 green, E3 passed (P22–P34) |
| Windows program `ProsperoAutoDrive.exe`: tray, drive, pairing, re-pairing, adopting a new server, stopping the server, GOODBYE in the log | E1 + E2 green, E3 passed (P32, P3.3) |
| Installation and removal (user folder and protected folder) | E3 passed (P3.3) |
| Everything in English, no personal data in the repo or its history, CI guard `check-sanitized.py` | E2 (CI) |
| Clock: the +1 h came from a wrong PS5 time zone; after correcting it the skew was −2 s | E3 |

The last green build at the time of writing is `548eadb` (docs only on top of `4cf0b48`).

### Decisions of the project owner on 0.1.0 (2026-10-25)

- **P33 belongs to 0.1.0.**
- **The release is built only when the project owner gives the go.** Prepare everything, never trigger it on your own.
- Versioning: 0.1.0 now, then SemVer patch/minor (0.1.1, 0.2.0 …) until G2 and 1.0.0.

### Open questions to the project owner (ask them before the affected task)

| # | Question | Recommendation | Needed for |
|---|---|---|---|
| Q1 | Should the timer fix (T1) go into 0.1.0? | Yes: small, PS5 side only, tested together with P33 | T1 |
| Q2 | Which files go into the release? | Only `ProsperoAutoDrive.exe`, `prospero-smbd.elf`, checksums, license texts, notices and the source archive. No test programs, no spike payloads | T5 |
| Q3 | Mark 0.1.0 as a GitHub "pre-release" or as a normal release? | Normal release: then `/releases/latest` works, and 0.x already signals that it is early | T5 |
| Q4 | Who starts the release? | The project owner clicks "Run workflow" in the Actions tab. That click is the go | T5, T6 |
| Q5 | Where is the ELF file registered in the Payload Manager on the PS5? | A folder of its own, e.g. `/data/prospero/`, never `/data/homebrew` | T2 |
| Q6 | Attach a full source archive (including the libsmb2 submodule) to the release? | Yes, because of GPL-3.0 for the PS5 payload (see T4) | T4, T5 |

### Order

```
T1 timer fix ──┐
T2a pid/ppid ──┴─> CI green ─> owner: P32 smoke + P33 (T2) ──┐
T3 user guide ───────────────────────────────────────────────┤
T4 notices ──────────────────────────────────────────────────┼─> freeze commit ─> CI green ─> owner: final P32 smoke
T5 release mechanism (with dry run in CI) ───────────────────┘                                     │
                                                                          owner's go ─> T6 PR, merge, release
```

T1 and T2a go into **one** build, so that the project owner tests only once. T3–T5 can be done while waiting for the owner.

## 1. Working rules that matter here

They come from `CLAUDE.md` and from experience; read `CLAUDE.md` in full first.

- **Communication:** replies to the project owner in German, test steps spelled out one by one with what they should see. Code, commits and documents in English. State the evidence level with every status.
- **The owner tests on another PC** and gets everything only via GitHub: commit, push to `dev`, wait for the CI. Ask for a test only when the CI is green on **exactly** that commit. `tools\Run-Test.cmd` refuses if the build commit differs from the checkout, so every push, even docs only, means waiting for the CI again before a test.
- **Never push to `main`.** Commit identity is the repo-local GitHub noreply address. Commits end with the `Co-Authored-By` line.
- **The development machine:** follow the agent memory on what may run locally. In short: build and unit-test only; do not install, register or start the product or test programs there, and send nothing to the PS5 without the owner's explicit word.
- **PS5 data:** test writes only in new `/data/_ProsperoAutoDrive_Test_<timestamp>-<random>` folders; never read or change `/data/homebrew` or existing folders in tests; after an unclear write result only check read-only.
- **CI status without a login:** the anonymous GitHub API allows only 60 requests per hour. Poll `https://github.com/<owner>/<repo>/releases/download/dev-build/BUILD-INFO.txt` until `commit=` is the pushed commit. Failing jobs post a log tail as an annotation, which the run page shows without login.
- **No `gh` CLI locally.** The PR and the release workflow are started by the project owner in the GitHub web interface.
- **Known traps:**
  - PS5 SDK: `libc.a` contains bare syscall stubs that do not set `errno` and may call something else on the console. Use only libkernel exports. The CI step `check-raw-syscalls.sh` checks this; do not add libc calls that are not yet proven on the console.
  - GCC in the host CI warns about `snprintf` truncation where local clang does not; match buffer sizes.
  - Shell heredocs in the Bash tool collapse `\\`. Edit `.cmd` and C files with the Edit tool.
  - The Payload Manager dashboard always starts the **registered file**. After each new build, that file on the PS5 must be replaced, or the old version runs.

## 2. Tasks

### T1 – Server timers independent of the PS5 clock

**Why:** setting the PS5's clock while the server runs, as the project owner did on 2026-10-25, moves timers that use the wall clock:

- Clock moved back by 1 h: the pairing window after a start stays open for about 1 h instead of 120 s, and idle service connections are closed only after about 1 h instead of 60 s.
- Clock moved forward: the pairing window closes at once, and every idle connection is dropped at once.

**Affected code:** every use of `time(NULL)` or `gettimeofday` for a duration or a deadline:

| File | Place | Use |
|---|---|---|
| `ps5/smbd/main.c` | `pairing_is_open`, `on_open_pairing`, pairing after the start | deadline `g_pairing_until` |
| `ps5/smbd/main.c` | `g_start`, `uptime_s` in `status()` | duration |
| `ps5/smbd/main.c` | `round` in the outer loop | duration (≥ 60 s healthy) |
| `ps5/smbd/service.c` | `c->last` (2×), `now` in `service_run` | idle timeout `IDLE_SECONDS` |
| `ps5/smbd/diag.c` | `now_ms()` | loop gap, a duration |

**Stays on the wall clock** (correct as it is): log time stamps (`log.c`), `now_utc` in the status, the clock skew from PING (`on_clock`), file times in the backend.

**Plan:**

1. Move `mono_s()` from `backend.c` into a small header, add a millisecond variant for `diag.c`.
2. Replace the uses in the table.
3. Add host test case 6b (below). If possible, push the test first: it must fail on the old code and pass after the fix. A red intermediate commit only means no new `dev-build`; the last green one stays.

**Mock code:**

```c
/* ps5/smbd/monotime.h */
#ifndef MONOTIME_H
#define MONOTIME_H
#include <stdint.h>
#include <time.h>

/* Time since an arbitrary start that does not move when someone sets the
 * console's clock. For timers and durations only; log lines, now_utc, the
 * clock skew and file times stay on the wall clock. CLOCK_MONOTONIC is
 * proven on the console (name cache, signing check). */
static inline time_t
mono_s(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1;       /* never 0, so 0 can mean "not set" */
}

static inline int64_t
mono_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif
```

```c
/* main.c, sketch */
static int
pairing_is_open(void) {
  return g_cfg.npaired == 0 || mono_s() < g_pairing_until;
}
/* on_open_pairing:   g_pairing_until = mono_s() + PAIRING_SECONDS;     */
/* after the start:   g_pairing_until = mono_s() + PAIRING_ON_START_S;  */
/* g_start = mono_s(); status(): uptime_s = mono_s() - g_start,
 * now_utc still from time(NULL)                                         */
/* outer loop: time_t round = mono_s(); ... if(mono_s() - round >= 60)   */

/* service.c: c->last = mono_s(); and in service_run: time_t now = mono_s(); */
/* diag.c: now_ms() returns mono_ms()                                    */
```

**Watch out for:**

- `g_pairing_until = 0` means "closed". That stays correct because `mono_s()` is never 0.
- Keep `backend.c` using the shared header, so there is one definition.
- Do not touch the clock-skew code. It must keep comparing the PC's UTC with `time(NULL)`.
- `check-raw-syscalls.sh` must stay green. `clock_gettime` is already in use, so nothing new is expected.

**Test E1, new case 6b in `tests/smbd/host-test.sh`**, placed after case 6a and before case 7. At that point the state has paired PCs, so pairing is open for 120 s after each start. The wall clock of the server process alone is shifted with libfaketime; the monotonic clock is left alone.

```bash
echo "== case 6b: timers ignore changes of the wall clock"
FT=$(dpkg -L faketime 2>/dev/null | grep '/libfaketimeMT\.so\.1$' | head -1)
need test -n "$FT"                      # CI: apt-get install faketime
echo "+0" > "$WORK/faketime"
# Only the server gets the preload, not curl or python in the checks.
start_with env LD_PRELOAD="$FT" FAKETIME_TIMESTAMP_FILE="$WORK/faketime" \
  FAKETIME_NO_CACHE=1 DONT_FAKE_MONOTONIC=1
expect "$(status)" '^pairing=open$'
# A service connection that stays open across the jump (as the agent's).
python3 - "$WORK/idle.txt" <<'PY' &
import socket, sys, time
s = socket.create_connection(("127.0.0.1", 4445), timeout=10)
f = s.makefile("rb")
s.sendall(b"HELLO 1\n"); f.readline()
time.sleep(3)                            # the jump happens meanwhile
s.sendall(b"PING 1 utc_ms=1\n")
open(sys.argv[1], "w").write(f.readline().decode())
PY
idle=$!
sleep 1
echo "+2h" > "$WORK/faketime"           # wall clock 2 hours forward
sleep 1
out=$(status)
expect "$out" '^pairing=open$'          # the wall clock would have closed it
need [ "$(sed -n 's/^uptime_s=//p' <<<"$out")" -lt 60 ]
need wait "$idle"
expect "$(cat "$WORK/idle.txt")" '^PONG 1 '   # not dropped as idle
svc 127.0.0.1 "QUIT 1" >/dev/null
need wait "$server"
server=
echo "case 6b passed"
```

- `start_with` is a variant of `start()` that puts the given command prefix in front of `"$BIN"`. Do **not** call `VAR=x start`: inside the function the variables would also reach `curl` in `status`.
- libfaketime works only with a dynamically linked binary. The host build is dynamic; check it with `ldd` in the test, or skip the case loudly if not.
- Use `libfaketimeMT` because the server has a write thread. Check the path and the environment variables against the installed version (`dpkg -L faketime`, `man faketime`); Ubuntu 24.04 ships 0.9.10.
- Add `faketime` to the `apt-get install` line of the `ps5-host-test` job in `ci.yml`.

**Test E3:** no separate clock jump on the PS5 is needed: CLOCK_MONOTONIC is already proven there. The regression proof is P32 plus P33 on the build with T1: `uptime_s` grows plausibly and pairing closes as before. Optionally, if the project owner wants, they set the PS5 clock 1 h forward right after a restart from the dashboard; then `pairing=open` must stay until about 120 s after the start. Afterwards they set the clock back.

**Done when:**

- Case 6b passes in the CI and failed on the old code, or the reason it could not be shown is written down.
- All other host test cases are unchanged and green.
- P32 passes E3 on the build.
- `decisions.md` has a short entry: timers on the monotonic clock, why, and what stays on the wall clock.

### T2a – Show the process in the status (helper for P33)

**Why:** the short P33 probe could not answer whether the server runs as a process of its own. It did not appear under "Active Processes" in the Payload Manager. The process ID and the parent's ID answer that directly: started via websend, the server runs inside `pldmgr.elf`; started from the file, it should have an ID of its own.

**Mock code** (`main.c`, `status()`), after `instance=`:

```c
"pid=%d\nppid=%d\n", (int)getpid(), (int)getppid(),
```

**Watch out for:** `getpid` and `getppid` must be libkernel exports. `check-raw-syscalls.sh` fails otherwise; then drop `ppid` or find the libkernel name. Scripts read the status line by line by key, so additional lines break nothing; check `Run-Test.cmd` and the P-check scripts anyway.

**Test:** E1, case 1 checks `^pid=[0-9]+$` and `^ppid=[0-9]+$`. E3 via P33.

**Done when:** CI green; the status on the PS5 shows both values.

### T2 – Full P33 test: the payload as its own process (E3, project owner)

**Steps and expectations:** see [roadmap P33](roadmap.md#p33--payload-as-its-own-process) (P33.1–P33.7). The short probe of 2026-10-25 passed P33.6 and P33.1 in part.

**Plan:**

1. Write `tests/smbd/P33-Check.cmd`, modelled on `P32-Check.cmd`:
   - It starts `ProsperoAutoDrive.exe` from `dist\dev` with its own `--state-dir` in `%TEMP%`.
   - It guides the project owner through each step with a prompt on the console (`>con`), e.g. "Start the server from the dashboard, then press Enter".
   - It writes a time stamp to the log for each step and waits for the expected log line (` connected as `, ` disconnected`), with a time-out.
   - After each step it fetches the status and logs `instance=`, `pid=`, `ppid=` and `uptime_s=`. A new `instance` proves a new start; `pid`/`ppid` answer P33.1/P33.4.
   - At the end it prints the program's log and `P33-CHECK: COMPLETED`.
2. Add a `P33` entry to `tools\Run-Test.cmd`. **Important:** for P33, Run-Test must **not** send the payload via websend in steps 2–4. The server is started from the Payload Manager. A flag such as `NOSEND=1` skips sending and the "ready" wait, or replaces it with "start the server from the dashboard now".
3. Before the run, tell the project owner (in German, step by step):
   - which file to copy from `dist\dev` to the registered place on the PS5 (Q5), via FTP or via `P:`;
   - that the dashboard otherwise starts the old file.
4. Afterwards: write the results into the P33 section of `roadmap.md` (E3, build, date) and note open points.

**Watch out for:**

- P33.2 says "stop via Active Processes". The short probe did not show the server there. If it stays like that, use the tray item "Stop <name>..." instead and record the deviation; do not make up a pass.
- P33.5 (autoload after a restart of the PS5): the drive comes only after the jailbreak and the Payload Manager have started, not seconds after power-on. Measure from the "running" notification.
- The PS5's IP address may change after a restart (DHCP). The program then searches by broadcast; that is expected behavior, not an error.
- The script writes nothing on the PS5. The project owner copies the ELF by hand; that is their action, not a test write.
- Progress shows on the console (`>con`), never in the pasted log.

**Done when:** P33.1–P33.7 each passed E3 or have a written, accepted deviation. Results are in the roadmap.

### T3 – User guide in the README

**Why:** the README explains what the project is, but not how to set it up, use it and remove it. The release page links to it.

**Plan:** add a section "Getting started" or a page `docs/user-guide.md`, linked from the README and the release notes. **Every statement must be checked against the code** (`windows/app/ui.cpp`, `setup.cpp`, `main.cpp`, `ps5/smbd/main.c`), not taken from memory or from this plan. Content:

1. **Requirements:** a PS5 with a jailbreak and the Payload Manager (or another ELF loader); Windows 11 (Windows 10 22H2 best effort); both in the same network; port 445 not blocked.
2. **PS5 setup:**
   - Set the time zone in Settings → System → Date and Time, then the time by hand. The program corrects remaining seconds; a wrong time zone shifts all PS5 times by hours.
   - Copy `prospero-smbd.elf` to a folder of its own (not `/data/homebrew`).
   - Register it in the Payload Manager and add it to "Autoload".
   - The server keeps its state in `/data/prospero-smbd/` (`config.ini`, log). Deleting `config.ini` resets it: new identity and new password, and all PCs must pair again.
3. **PC setup:**
   - Download from the release page and optionally compare the checksum: `certutil -hashfile ProsperoAutoDrive.exe SHA256` against `SHA256SUMS`.
   - The program is not signed yet. SmartScreen shows "Windows protected your PC" → "More info" → "Run anyway".
   - Choose an installation folder. A protected folder such as `C:\Program Files` asks once for admin rights; a user folder does not. The program never runs with admin rights in operation.
   - The first PC that reports pairs automatically. Further PCs: "Pair another PC" on a paired PC, then start the program on the new PC within 10 minutes.
4. **Tray menu:** every item with one sentence, with the exact texts from `ui.cpp` (Open drive, Search the network …, Pair another PC, Pair with the new server..., Pair again, stop the server, Start with Windows, Show log, Exit).
5. **Removal:** Windows Settings → Apps → Prospero AutoDrive → Uninstall. Check in `setup.cpp` what is removed (files, autostart, uninstall entry, credentials?) and write exactly that. On the PS5: remove it from the Payload Manager and delete the ELF and `/data/prospero-smbd/`.
6. **Troubleshooting:**
   - Where the log is ("Show log").
   - The drive does not appear: is the PS5 reachable, is port 445 blocked?
   - "Pairing refused": restart the server in the dashboard (pairing is open for 120 s after each start), then "Pair again".
   - "A different server": "Pair with the new server...".
7. **Known limitations:**
   - Not signed.
   - The whole PS5 file system is shared read-write; mistakes there can damage the console's data.
   - The first PC that reports gets the password; that risk in the user's own network is the user's.
   - Windows 10 best effort.
   - Throughput gate G2 still open.
   - A small file can take about 1 s during a large copy.

**Watch out for:** no personal data, no IP addresses of real devices, no user names in paths (use `C:\Users\<name>`); the CI guard checks this. Screenshots only if the project owner provides them.

**Done when:** every item above is covered, every statement was checked against the code, the CI is green, and the project owner has read the guide (ask them to).

### T4 – License texts and notices for third-party code

**Why:** the binaries contain third-party code, and its licenses require notices or source code.

**Plan:**

1. **Find out what is linked**, without guessing:
   - Windows: in the CI, link `ProsperoAutoDrive.exe` once with `-Wl,--trace` or `-Wl,-Map=...` and post the list of archives as an annotation. Expected: libc++, libc++abi, libunwind, compiler-rt builtins, mingw-w64 CRT (`libmingw32`, `libmingwex`, UCRT import libraries), possibly `winpthreads`.
   - PS5: libsmb2 (submodule plus `third_party/patches`), libraries of the ps5-payload-sdk v0.44 (crt, libc, libkernel stubs).
2. **Read the licenses** in the sources: `third_party/libsmb2/COPYING` and `LICENCE-LGPL-2.1.txt`, the LICENSE of the SDK, `COPYING.MinGW-w64-runtime.txt` of mingw-w64, LLVM `LICENSE.TXT` (Apache-2.0 WITH LLVM-exception).
3. **Write `THIRD-PARTY-NOTICES.txt`** in the repo root, as text:
   - each component with version or commit, license, and the license text or a reference to a bundled file;
   - for libsmb2, also the note that it is modified, and where the patches are.
4. **GPL source:** `prospero-smbd.elf` is GPL-3.0. GitHub's automatic "Source code" archives do **not** contain submodules. So the release gets its own source archive: the repo at the tag including `third_party/libsmb2` (Q6). For the SDK, name the exact version and its download address with SHA-256, as pinned in `ci.yml`.
5. Add the notice file and the license texts to the release files (T5).

**Watch out for:** this is not legal advice. Where a license is unclear, put it to the project owner with options; do not decide it alone. Do not commit license texts that are binary or larger than 1 MiB (CI guard).

**Done when:** the notice file covers every linked component found in step 1, and the release dry run (T5) shows it among the files.

### T5 – Release mechanism

**Rule from `CLAUDE.md`:** a release publishes the hash-identical artifacts of the tested `dev` build with the same Git tree hash. No rebuild.

**Design:**

- **One script** `.github/scripts/release-assemble.sh <version> <out dir>`. It collects and checks, and builds nothing:
  1. Download `dev-build` and verify every file against `SHA256SUMS`.
  2. `tree=` from `BUILD-INFO.txt` must equal `git rev-parse HEAD^{tree}`. The build commit must be an ancestor of `HEAD` (`git merge-base --is-ancestor`).
  3. The version must be the same in `CMakeLists.txt` (`project(... VERSION x.y.z)`) and `windows/app/app.rc` (FILEVERSION, PRODUCTVERSION, both strings), and must match the requested version. The tag `v<version>` must not exist yet.
  4. Copy the product files (Q2); take their lines from the tested `SHA256SUMS` unchanged; add the license texts, `THIRD-PARTY-NOTICES.txt`, a `BUILD-INFO.txt` with version, commit, tree and CI run, and the source archive (T4); append checksums for the added files.
- **Dry run in the normal CI:** a job `release-dry-run` in `ci.yml`, after `publish-dev`, only on `dev`. It runs the script with the version from `CMakeLists.txt` and uploads the result as an Actions artifact, without publishing anything. That tests the script on every push.
- **Release workflow** `.github/workflows/release.yml`: `workflow_dispatch` with input `version`. It runs only on `main` and calls the same script, then `gh release create`.

**Mock code:**

```bash
#!/usr/bin/env bash
# Assembles a release from the tested dev build. Builds nothing: every
# product file is the one the project owner tested, with its checksum.
# Usage: release-assemble.sh <version> <out dir>   (needs GH_TOKEN, git with tags)
set -euo pipefail
VERSION="${1:?version}"; OUT="${2:?out dir}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "::error::bad version $VERSION"; exit 1; }
PRODUCT=(ProsperoAutoDrive.exe prospero-smbd.elf)
dl="$(mktemp -d)"
gh release download dev-build --dir "$dl"
(cd "$dl" && sha256sum --strict -c SHA256SUMS)
info() { sed -n "s/^$1=//p" "$dl/BUILD-INFO.txt"; }
[ "$(info tree)" = "$(git rev-parse 'HEAD^{tree}')" ] ||
  { echo "::error::dev-build tree $(info tree) is not the tree of HEAD - was dev changed after the test?"; exit 1; }
git merge-base --is-ancestor "$(info commit)" HEAD ||
  { echo "::error::dev-build commit is not part of HEAD"; exit 1; }
grep -q "^project(ProsperoAutoDrive VERSION $VERSION " CMakeLists.txt ||
  { echo "::error::CMakeLists.txt is not version $VERSION"; exit 1; }
grep -q "VALUE \"ProductVersion\", \"$VERSION\"" windows/app/app.rc ||
  { echo "::error::app.rc is not version $VERSION"; exit 1; }
# (also FILEVERSION/PRODUCTVERSION x,y,z,0 and FileVersion)
if git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null; then
  echo "::error::tag v$VERSION exists already"; exit 1
fi
mkdir -p "$OUT"
: > "$OUT/SHA256SUMS"
for f in "${PRODUCT[@]}"; do
  cp "$dl/$f" "$OUT/"
  grep -E "^[0-9a-f]{64}  $f\$" "$dl/SHA256SUMS" >> "$OUT/SHA256SUMS"
done
cp LICENSE "$OUT/LICENSE.txt"
cp ps5/LICENSE "$OUT/LICENSE-ps5.txt"
cp THIRD-PARTY-NOTICES.txt "$OUT/"
{ echo "version=$VERSION"; echo "commit=$(info commit)"; echo "tree=$(info tree)"; echo "dev_run=$(info run)"; } \
  > "$OUT/BUILD-INFO.txt"
# Source archive including the submodule (GPL-3.0 for the payload), see T4.
src="prospero-autodrive-$VERSION-source"
git archive --prefix="$src/" -o "$dl/main.tar" HEAD
git -C third_party/libsmb2 archive --prefix="$src/third_party/libsmb2/" -o "$dl/libsmb2.tar" HEAD
tar --concatenate -f "$dl/main.tar" "$dl/libsmb2.tar"
gzip -n -9 < "$dl/main.tar" > "$OUT/$src.tar.gz"
(cd "$OUT" && sha256sum -- LICENSE.txt LICENSE-ps5.txt THIRD-PARTY-NOTICES.txt BUILD-INFO.txt "$src.tar.gz" >> SHA256SUMS)
cat "$OUT/BUILD-INFO.txt" "$OUT/SHA256SUMS"
```

```yaml
# .github/workflows/release.yml
name: Release
on:
  workflow_dispatch:
    inputs:
      version:
        description: "Version to release, e.g. 0.1.0 (must match the source)"
        required: true
permissions:
  contents: read
concurrency:
  group: release
  cancel-in-progress: false
jobs:
  release:
    if: github.ref == 'refs/heads/main'
    runs-on: ubuntu-24.04
    permissions:
      contents: write
    env:
      VERSION: ${{ inputs.version }}     # used only via env, never inline in run:
      GH_TOKEN: ${{ github.token }}
    steps:
      - uses: actions/checkout@v4
        with: { fetch-depth: 0, submodules: true }
      - run: bash .github/scripts/release-assemble.sh "$VERSION" out
      - run: |
          gh release create "v$VERSION" out/* --target "$GITHUB_SHA" \
            --title "Prospero AutoDrive $VERSION" --notes-file "docs/release-notes/$VERSION.md"
      # then: check, as publish-dev does, that the release is public and
      # that BUILD-INFO.txt downloads anonymously with the right commit
```

**Watch out for:**

- **`workflow_dispatch` only works for workflows on the default branch.** `release.yml` therefore becomes usable only after the merge to `main`. That is why the dry run in `ci.yml` on `dev` matters: it is the only way to test the script beforehand.
- **Freeze:** `dev-build` is replaced by every green `dev` push. Between the project owner's final test and the release, nothing may be pushed to `dev`, or the tree check refuses, which is correct. If that happens, run the final smoke test again.
- **Tree after the merge:** the ruleset on `main` allows only merge commits. The merge commit has the same tree as `dev` only if `main` is an ancestor of `dev`. Check `git merge-base --is-ancestor origin/main origin/dev` before the PR; if it fails, merge `main` into `dev` first and test again.
- **Workflow inputs:** pass them via `env` and validate them in the script. Never write `${{ inputs.version }}` directly into a `run:` line (script injection).
- **Release notes:** `docs/release-notes/0.1.0.md`, in English. Content: what 0.1.0 can do; links to the user guide (T3); the files with their purpose; how to check the checksums; known limitations; the evidence (E3 tests with build and date).
- **Draft trap:** a release once stayed invisible (see `publish-dev`). Check the anonymous download at the end, as `publish-dev` does.
- **After the release:** set the version on `dev` for the next release (CMakeLists.txt and app.rc together) only when the project owner decides what comes next.

**Test:**

- The `release-dry-run` job is green on `dev`.
- Its artifact contains exactly the expected files.
- The product files have byte for byte the checksums of `dev-build`.
- Negative checks in the job, without publishing: a wrong version and a wrong tree are refused. For example, run the script once with a version that does not match and expect a failure.

**Done when:**

- The dry run is green and checked.
- `release.yml` is on `dev` and has been reviewed.
- The release notes are written.
- The project owner knows the click path for T6.

### T6 – Go, PR, merge, release (with the project owner)

**Preconditions:** T1–T5 done. All results are in the roadmap. The freeze commit is pushed and the CI is green on it, including `release-dry-run`.

**Steps:**

1. **Final smoke test (E3):** the project owner runs `.\tools\Run-Test.cmd P32` on the freeze commit. That build is the one that will be released.
2. **Go:** ask the project owner explicitly and wait for their answer. No go, no release.
3. **PR:** the project owner opens `https://github.com/<owner>/<repo>/compare/main...dev`, "Create pull request", title "Release 0.1.0". Give them the description text (English, with the attribution line). The CI must be green, including `guards`.
4. **Merge:** "Create a merge commit" (only option allowed by the ruleset). After that, nothing is pushed to `dev` until the release exists.
5. **Release:** the project owner opens Actions → "Release" → "Run workflow", branch `main`, version `0.1.0`.
6. **Check (E2 + E3):**
   - Download every release asset anonymously and verify it against `SHA256SUMS`.
   - Compare the product checksums with `dev-build`.
   - Check that `/releases/latest` points to 0.1.0, if it is not a pre-release (Q3).
   - The project owner downloads `ProsperoAutoDrive.exe` from the release page once and starts it.
7. **Afterwards:** add the release with date, tag and commit to `roadmap.md` (on `dev`).

**Done when:** the release `v0.1.0` is public, its product files are hash-identical with the tested build, and the roadmap records it.

## 3. Definition of done for 0.1.0

- [ ] T1 timer fix: E1 (case 6b) and E3 (P32) passed, or the project owner declined it (Q1).
- [ ] T2 P33.1–P33.7: E3 passed or accepted deviation recorded.
- [ ] T3 user guide checked against the code and read by the project owner.
- [ ] T4 notices for all linked components; source archive in the release.
- [ ] T5 dry run green; release workflow on `main`.
- [ ] Final P32 smoke test (E3) on exactly the released tree.
- [ ] The project owner's go.
- [ ] Release `v0.1.0` public, hash-identical with `dev-build` of the tested commit.
- [ ] Roadmap updated.

## 4. Not part of 0.1.0

Phase 4 and later, see the [roadmap](roadmap.md):

- signing, Smart App Control, and the visible UAC prompt with default UAC;
- G2 throughput, Wi-Fi optimization, and the wait under load;
- 7 days of operation and the Windows 10 check;
- the remaining smbtorture tests.
