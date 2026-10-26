# Prospero AutoDrive

> **Status:** pre-release (0.x). Usable for testing; the interfaces may still change before 1.0.0.

Prospero AutoDrive shows the file system of a PS5 that runs a homebrew payload as a normal network drive (`P:`) in Windows Explorer. The drive connects automatically as soon as the console is reachable and is removed when it goes offline. Set it up once and never touch it again.

Windows needs nothing but its built-in SMB client and one small program. No driver, no third-party software, and no admin rights in operation.

## Components

- **`prospero-smbd.elf`** (`ps5/`): an SMB2/3 server payload for the PS5, based on the libsmb2 server engine with its own POSIX file system backend. NTLMv2 with mandatory signing, one read-write share. A service channel on TCP 4445 carries heartbeats, time synchronisation and pairing; discovery runs on UDP 4445.
- **`ProsperoAutoDrive.exe`** (`windows/`): a small Win32 tray program for Windows 11 (Windows 10 22H2 best effort). It is its own installer and uninstaller, pairs with the server once, keeps the credentials in the Windows Credential Manager and maps the drive with the built-in SMB client while the server is reachable.

## How it works

1. Upload `prospero-smbd.elf` in the Payload Manager under "Manage Payloads" (it keeps it in `/data/pldmgr/payloads/prospero-smbd/`, where the server also keeps its state) and start it from there, ideally with autoload.
2. Start `ProsperoAutoDrive.exe` on the PC and choose an installation folder. It finds the server on the local network, pairs with it and maps the drive.
3. From then on the drive appears and disappears with the console. The tray menu shows the state and offers actions such as searching the network, pairing another PC, re-pairing and ending the server.

**Getting started:** the [user guide](docs/user-guide.md) describes setup, the tray menu, removal, troubleshooting and the known limitations. Downloads are on the [release page](https://github.com/20913Tdog/ProsperoAutoDrive/releases/latest).

## Building

One toolchain for all targets: llvm-mingw (Clang) for the PS5 and Windows, the ps5-payload-sdk, CMake and Ninja; no MSVC. Builds use the CMake presets `ps5`, `windows` and `host` (Linux tests). The pinned versions are in `.github/workflows/ci.yml`. The CI builds and tests every commit; release binaries come from the CI, never from a local build.

## Documentation

- [`docs/user-guide.md`](docs/user-guide.md): setup, use, removal and troubleshooting
- [`docs/roadmap.md`](docs/roadmap.md): phases, status and test results
- [`docs/decisions.md`](docs/decisions.md): design decisions and their reasons
- [`docs/design-phase2.md`](docs/design-phase2.md), [`docs/design-phase3.md`](docs/design-phase3.md): designs of the server and the Windows program
- [`tests/spikes/README.md`](tests/spikes/README.md): feasibility spikes S1–S7 and their results

## Licensing

- Repository root, Windows components, tests and documentation: [MIT](LICENSE)
- PS5 payload (`ps5/`): GPL-3.0, see [`ps5/LICENSE`](ps5/LICENSE)
- Third-party code in the release binaries: [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt)
