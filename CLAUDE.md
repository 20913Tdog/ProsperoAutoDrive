# CLAUDE.md – Prospero AutoDrive

Projektregeln für die Entwicklung. Diese Regeln sind verbindlich und haben Vorrang vor allgemeinen Gewohnheiten.

## Zweck

Ein Windows-PC zeigt das Dateisystem einer PS5, auf der ein Homebrew-Payload läuft, als normales Explorer-Laufwerk `P:`. Das Laufwerk erscheint automatisch, sobald die PS5 erreichbar ist, und verschwindet, sobald sie offline geht. Es wird einmal eingerichtet und danach nie wieder angefasst.

## Architektur (entschieden, Neustart vom 20.10.2026)

- **PS5:** eigenes Payload `prospero-smbd.elf`. Es ist ein SMB2/3-Server auf Basis der libsmb2-Server-Engine (gepinnter `master`-Commit) mit eigenem, protokollneutralem POSIX-Dateisystem-Backend. Es nutzt NTLMv2, immer Signing, vergibt keine Oplocks/Leases und schreibt synchron. Es gibt genau eine Freigabe `/`, lesen und schreiben, ohne Schutzliste. Lizenz GPL-3.0 (PS5-SDK), im Verzeichnis `ps5/`.
- **Windows:** nur der eingebaute SMB-Client plus `ProsperoAutoDrive.exe`, in C++/Win32, gebaut mit llvm-mingw und statisch gelinkt (nur Windows-eigene DLLs plus UCRT), MIT-Lizenz, im Verzeichnis `windows/`. Das Programm hat immer ein Tray-Symbol und benachrichtigt bei Verbinden, Trennen und Problemen. Es nutzt temporäre Mappings (`WNetAddConnection2`/`WNetCancelConnection2`) und bewahrt Zugangsdaten in der Windows-Anmeldeinformationsverwaltung auf. Es braucht keinen Admin, keinen Treiber und keine Fremdsoftware. **Das Windows-Programm bleibt generisch**: keine PS5- oder Jailbreak-spezifischen Funktionen, damit eine kostenlose Signierung später möglich bleibt.
- **Verbinden und Trennen:** UDP-Liveness mit Server-ID. Wegen WLAN wird erst getrennt, wenn UDP-Liveness **und** ein TCP-445-Test fehlschlagen.
- **Uhrzeit:** Die interne UTC-Uhr der PS5 kann abweichen (gemessen: +1 h). Das Programm schickt in der UDP-Liveness die UTC-Zeit des PCs mit. Der Server misst daraus die Abweichung und rechnet Zeitstempel in beide Richtungen um.
- **Ziele:** Windows 11 (Home/Pro/Enterprise, Standard-Sicherheit) voll unterstützt, Windows 10 22H2 Best Effort. Port 445 ist der Primärpfad.
- **Vorgehen:** Vor jedem Produktcode laufen die Spikes S1–S7 (S5 nur simuliert, Entscheidung vom 21.10.2026) und Gate G1. G1 verlangt Durchsatz ≥ 70 % von ftpsrv, G2 (Release 1.0.0) verlangt ≥ 95 %, gemessen im WLAN, je Richtung ≥ 7 Läufe abwechselnd, Median.

## Harte Regeln

### Daten auf der PS5

- Automatisierte und Test-Schreiboperationen **ausschließlich** in neu erzeugten Ordnern `/data/_ProsperoAutoDrive_Test_<Zeitstempel>-<Zufall>`.
- `/data/homebrew` und alle bereits bestehenden Verzeichnisse in Tests **weder lesen noch verändern**.
- Nach einem unklaren Ergebnis einer Schreiboperation nur **lesend** prüfen. Keine automatische Wiederholung, keine automatische Bereinigung, keine breitflächigen Löschungen.

### Toolchain

- Es gibt eine Toolchain für alle Ziele, lokal und in der CI mit identischen Versionen. Sie liegen gepinnt in `.github/workflows/ci.yml`:
  - **llvm-mingw** `20260922` (Clang 23) für PS5 und Windows
  - **ps5-payload-sdk** `v0.44`
  - CMake + Ninja
- Kein MSVC und kein Visual Studio. Es gibt keine Lizenzfragen, und es braucht keinen Installer.
- Gebaut wird über CMake-Presets: `ps5`, `windows`, `host` (Linux-Tests). Maschinenspezifische Pfade stehen in der nicht versionierten `CMakeUserPresets.json`, etwa `ps5-local` und `windows-local`.
- Lokal (Entwicklungsrechner, ohne Admin): portable Ordner in `%LOCALAPPDATA%\Programs` (`llvm-mingw-20260922`, `ps5-payload-sdk-v0.44`, `cmake-4.4.4`). Ninja kommt aus dem SDK.
- Die Compiler-Wrapper des SDK werden nicht verwendet. Unter Windows hängen sie `crt1.o` doppelt an. Details stehen in `cmake/toolchains/ps5.cmake`.
- Lokale Builds sind schnelle Rückmeldung. Maßgeblich für Tests und Releases bleibt die CI.

### Git, Build, Release

- `main` ist stabil. **Niemals direkt auf `main` pushen.** Änderungen kommen nur per PR von `dev`, nach Live-Test und OK des Projektverantwortlichen.
- Alle Änderungen werden committet und auf `dev` gepusht. Der Projektverantwortliche testet auf einem anderen PC und kommt nur über GitHub an die Dateien.
- **Keine Binaries in Git** (CI-Guard). CI baut jeden `dev`-Commit; ein grüner Build ersetzt das Pre-Release `dev-build`, das `tools\Get-DevBuild.cmd` mit `curl.exe` abruft und per SHA-256 prüft. Der Name ist bewusst nicht `dev`: Ein Tag `dev` neben dem Branch `dev` wäre mehrdeutig.
- Ein Release veröffentlicht die hash-identischen Artefakte des getesteten `dev`-Builds mit gleichem Git-Tree-Hash. **Kein Rebuild beim Release.**
- SemVer: `0.x` bis Gate G2, dann `1.0.0`.
- Commit-Identität ist ausschließlich die GitHub-noreply-Adresse (repo-lokal gesetzt). **Niemals die globale bzw. Firmen-E-Mail verwenden.**

### Qualität und Kommunikation

- Bei jedem Status die Evidenzebene nennen: E1 Unit, E2 Protokoll-CI mit echtem Windows-Client, E3 PS5-Live. „Behoben“ nur mit Nachweis auf der geforderten Ebene.
- Den Projektverantwortlichen erst dann zum Testen auffordern, wenn CI auf **genau diesem** Commit grün ist.
- Kein PowerShell im Produkt. Der Projektverantwortliche braucht lokal keine Build-Werkzeuge und muss keine ZIPs entpacken.
- Kleine, zusammenhängende Änderungen. Vor jedem Push Syntax- und Unit-Prüfungen lokal ausführen, soweit möglich.
- Annahmen, die von der Norm abweichen und sinnlos erscheinen, aktiv hinterfragen, auch Vorgaben des Projektverantwortlichen. Kurz begründen, die übliche Praxis als Option anbieten, die finale Entscheidung respektieren.

## Lizenzen

- Repo-Root, Windows-Teil, Tests, Doku: MIT (`LICENSE`)
- `ps5/`: GPL-3.0 (eigene `LICENSE`)
