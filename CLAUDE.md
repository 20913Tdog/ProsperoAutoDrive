# CLAUDE.md – Prospero AutoDrive

Project rules for development. These rules are binding and take precedence over general habits.

## Purpose

A Windows PC shows the file system of a PS5 that runs a homebrew payload as a normal Explorer drive `P:`. The drive appears automatically as soon as the PS5 is reachable and disappears as soon as it goes offline. It is set up once and never touched again.

## Architecture (decided, restart of 2026-10-20)

- **PS5:** our own payload `prospero-smbd.elf`. It is an SMB2/3 server based on the libsmb2 server engine (pinned `master` commit) with its own protocol-neutral POSIX file system backend. It uses NTLMv2, always signs, grants no oplocks/leases and writes synchronously. There is exactly one share `/`, read and write, without a protection list. License GPL-3.0 (PS5 SDK), in the directory `ps5/`.
- **Windows:** only the built-in SMB client plus `ProsperoAutoDrive.exe`, in C++/Win32, built with llvm-mingw and statically linked (only Windows' own DLLs plus UCRT), MIT license, in the directory `windows/`. The program always has a tray icon and notifies on connect, disconnect and problems. It uses temporary mappings (`WNetAddConnection2`/`WNetCancelConnection2`) and keeps credentials in the Windows Credential Manager. In operation it needs no admin rights, no driver and no third-party software. Only the installation may ask for admin rights, when a protected folder is chosen (decision of 2026-10-24). **The Windows program stays generic**: no PS5- or jailbreak-specific functions, so that free code signing remains possible later.
- **Discovery:** UDP port 4445, the same number as the service channel (decision of 2026-10-21). The program first connects via TCP to the saved IP. Only if that fails does it search by UDP broadcast; as a last resort the user enters the IP. The reply gives server ID, name and ports.
- **Service channel:** TCP port 4445, for heartbeats with server ID, time and pairing (decision of 2026-10-21). Because of Wi-Fi, the drive is only disconnected when the heartbeat **and** a TCP 445 test fail.
- **Pairing:** The first PC that reports gets user and password over the service channel (variant C). A password is never displayed. The user bears the risk within their own Wi-Fi.
- **Time:** The PS5's internal UTC clock may deviate (measured: +1 h). The program sends the PC's UTC time over the service channel. From it the server measures the deviation and converts timestamps in both directions.
- **Targets:** Windows 11 (Home/Pro/Enterprise, default security) fully supported, Windows 10 22H2 best effort. Port 445 is the primary path.
- **Approach:** Before any product code, spikes S1–S7 run (S5 only simulated, decision of 2026-10-21) and gate G1. G1 requires throughput ≥ 70 % of ftpsrv, G2 (release 1.0.0) requires ≥ 95 %, measured over Wi-Fi, ≥ 7 alternating runs per direction, median.

## Hard rules

### Data on the PS5

- Automated and test write operations **only** in newly created folders `/data/_ProsperoAutoDrive_Test_<timestamp>-<random>`.
- In tests, **neither read nor modify** `/data/homebrew` or any directory that already exists.
- After an unclear result of a write operation, check **read-only**. No automatic retry, no automatic cleanup, no wide-ranging deletions.

### Toolchain

- There is one toolchain for all targets, locally and in the CI with identical versions. They are pinned in `.github/workflows/ci.yml`:
  - **llvm-mingw** `20260922` (Clang 23) for PS5 and Windows
  - **ps5-payload-sdk** `v0.44`
  - CMake + Ninja
- No MSVC and no Visual Studio. There are no license questions, and no installer is needed.
- Builds go through CMake presets: `ps5`, `windows`, `host` (Linux tests). Machine-specific paths are in the unversioned `CMakeUserPresets.json`, for example `ps5-local` and `windows-local`.
- Locally (development machine, without admin): portable folders in `%LOCALAPPDATA%\Programs` (`llvm-mingw-20260922`, `ps5-payload-sdk-v0.44`, `cmake-4.4.4`). Ninja comes from the SDK.
- The SDK's compiler wrappers are not used. On Windows they append `crt1.o` twice. Details are in `cmake/toolchains/ps5.cmake`.
- Local builds are quick feedback. The CI remains authoritative for tests and releases.

### Git, build, release

- `main` is stable. **Never push directly to `main`.** Changes come only by PR from `dev`, after a live test and the project owner's OK.
- All changes are committed and pushed to `dev`. The project owner tests on another PC and gets the files only through GitHub.
- **No binaries in Git** (CI guard). The CI builds every `dev` commit; a green build replaces the pre-release `dev-build`, which `tools\Get-DevBuild.cmd` fetches with `curl.exe` and verifies by SHA-256. The name is deliberately not `dev`: a tag `dev` next to the branch `dev` would be ambiguous.
- A release publishes the hash-identical artifacts of the tested `dev` build with the same Git tree hash. **No rebuild for the release.**
- SemVer: `0.x` until gate G2, then `1.0.0`.
- The commit identity is exclusively the GitHub noreply address (set locally in the repo). **Never use the global or company e-mail.**

### Quality and communication

- With every status, state the evidence level: E1 unit, E2 protocol CI with a real Windows client, E3 PS5 live. "Fixed" only with proof at the required level.
- Ask the project owner to test only when the CI is green on **exactly this** commit.
- No PowerShell in the product. The project owner needs no build tools locally and does not have to unpack ZIPs.
- Small, coherent changes. Before every push, run syntax and unit checks locally where possible.
- Actively question assumptions that deviate from the norm and seem pointless, including the project owner's requirements. Give a short reason, offer the usual practice as an option, respect the final decision.
- Replies to the project owner are in German; code, commits and documents are in English.

## Licenses

- Repository root, Windows part, tests, documentation: MIT (`LICENSE`)
- `ps5/`: GPL-3.0 (own `LICENSE`)
