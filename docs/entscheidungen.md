# Entscheidungen

Hier steht, was entschieden wurde und warum, damit es sich später nachvollziehen lässt. Der Projektverantwortliche trifft die Entscheidungen. Die verbindlichen Regeln für die Arbeit stehen in [`CLAUDE.md`](../CLAUDE.md).

Neue Einträge kommen unten dazu. Wird eine Entscheidung geändert, bleibt der alte Eintrag stehen und bekommt einen Hinweis auf den neuen.

Das ursprüngliche Planungsdokument mit dem Entscheidungsprotokoll vom 20.10.2026 lag nur lokal und wurde gelöscht. Die Einträge unten sind aus dem Arbeitsverlauf rekonstruiert.

## Neustart, 20.10.2026

### Architektur: natives SMB (D1)

Der alte Stack aus FTP, rclone und WinFsp (v1.x) wird verworfen. Ein eigenes PS5-Payload stellt einen SMB2/3-Server bereit. Windows nutzt nur seinen eingebauten SMB-Client und ein kleines Hilfsprogramm.

- **Warum:** FTP kann direktes Schreiben und die volle Explorer-Semantik nicht zugleich liefern, denn das VFS von rclone hat Grenzen. ftpsrv antwortet außerdem bei MKD, RMD, RNTO und DELE mit 226 statt mit 257 oder 250. Natives SMB braucht unter Windows keinen Treiber und keine Fremdsoftware.
- **Verworfen:**
  - **Alten Stack reparieren:** Das Zielbild bliebe unerreichbar.
  - **NFSv3:** Der Windows-Client hat nur Soft-Mounts mit kurzen Timeouts, Blöcke bis 32 KB und fehlt unter Home.
  - **WebDAV:** Der WebClient-Dienst ist abgekündigt und hat ein Dateilimit von etwa 50 MB.
  - **SFTP:** Auf der PS5 gibt es keinen SSH-Server, und WinFsp bliebe nötig.
- **Rückfallweg:** ein eigenes Protokoll mit eigenem WinFsp-Client, falls natives SMB am Gate G1 scheitert. Das Dateisystem-Backend wäre wiederverwendbar.
- **Grundlage:** die Server-Engine von libsmb2 (LGPL-2.1, statisch gelinkt, als Submodul auf einen Commit gepinnt) plus ein eigenes POSIX-Backend. Wegen des PS5-SDK steht das Payload unter GPL-3.0.

### Windows-Versionen (D2)

Windows 11 wird voll unterstützt: Home, Pro und Enterprise mit Standard-Sicherheit. Windows 10 22H2 ist Best Effort, weil es seit 10/2025 keinen regulären Support mehr hat.

### Freigabe und Laufwerk (D3, D3a, D3b)

- Eine Freigabe, Root `/`, lesen und schreiben. Sie umfasst `/data` und den Erweiterungsspeicher.
- Ein Laufwerk `P:` zeigt auf das Root. Statt dynamischer Freigaben gibt es Unterordner.
- **Keine Schutzliste** für Systempfade, wie bei FTP. Das Risiko ist bekannt und bewusst in Kauf genommen.

### Sicherheit

NTLMv2, Signing immer an, keine Oplocks und keine Leases, synchrones Schreiben. Windows 11 Pro ab 24H2 verlangt Signing und sperrt Gastzugriff, darum gibt es keinen Gastzugang.

### Windows-Programm (D4, D5, D8)

- **Sichtbarkeit:** Es gibt immer ein Tray-Symbol mit Status und Menü. Benachrichtigungen kommen bei Verbinden, Trennen und Problemen (P9).
- **Technik:** C++ und Win32, statisch gelinkt, MIT-Lizenz. Das Programm nutzt temporäre Laufwerksverbindungen und bewahrt Zugangsdaten in der Windows-Anmeldeinformationsverwaltung auf. Es braucht keinen Admin und keinen Treiber.
- **Signierung:** vorerst unsigniert, weil kein Budget da ist. Das Programm bleibt **generisch** und enthält keine PS5- oder Jailbreak-Funktionen. So bleiben die kostenlosen Wege offen: SignPath Foundation oder Microsoft Store. Geprüft wird das nach dem ersten 0.x-Release.
- **Gilt nicht mehr:** In D5 stand ursprünglich „MSVC in CI“. Ersetzt durch [Toolchain](#toolchain-llvm-mingw-statt-msvc).

### Verbinden und Trennen (P8)

Die PS5 hängt im WLAN. Das Programm prüft per UDP mit einer Server-ID, ob die PS5 erreichbar ist. Getrennt wird erst, wenn diese Prüfung **und** ein Test auf TCP-Port 445 fehlschlagen. WLAN-Aussetzer sind ein eigener Testfall. Port 445 ist der Primärpfad.

### Durchsatz-Gates (D7)

- **G1** (nach den Spikes): ≥ 70 % von ftpsrv
- **G2** (Release 1.0.0): ≥ 95 % von ftpsrv

Gemessen wird im WLAN, je Richtung mindestens 7 abwechselnde Läufe, entscheidend ist der Median.

### Vorgehen

Vor jedem Produktcode laufen die Spikes S1–S7 und das Gate G1.

### Repo, Branches und Releases (P1–P7, P11, P12)

- **P1:** Das alte GitHub-Repo wurde gelöscht und leer neu angelegt.
- **P2:** Alte Dokumente werden nicht übernommen.
- **P3, P4:** Keine Binaries in Git. Die CI baut jeden Commit auf `dev`, ein grüner Build ersetzt das Pre-Release `dev-build`. `main` ist stabil und ändert sich nur per PR nach Live-Test und OK des Projektverantwortlichen.
- **P5:** Ein Release veröffentlicht die hash-identischen Artefakte des getesteten `dev`-Builds. Es wird nicht neu gebaut.
- **P6:** Versionen 0.x bis Gate G2, dann 1.0.0. Eine Migration von 1.x gibt es nicht.
- **P7:** Name „Prospero AutoDrive“, technisch `ProsperoAutoDrive` und `prospero-smbd`.
- **P11:** Das Repo ist öffentlich. Das bringt kostenlose Actions-Minuten, Schutz für `main` im Free-Plan und eine CI, die ohne Login lesbar ist. Außerdem ist es Voraussetzung für SignPath.
- **P12:** Commits nur mit der GitHub-noreply-Adresse, nie mit der Firmen-E-Mail.
- **Geändert:** P3 sah das Pre-Release „dev“ vor. Es heißt jetzt `dev-build`, weil ein Tag `dev` neben dem Branch `dev` mehrdeutig wäre.
- **Überholt:** P2 sah vor, alte Dokumente nur im lokalen Archiv aufzubewahren. Dieses Archiv mit der Sicherung des alten Repos wurde gelöscht. Seitdem gibt es diese Datei hier.

### Arbeitsweise (P10)

Alle Änderungen kommen ins Repo, denn getestet wird auf einem anderen PC. Annahmen, die von der Norm abweichen, werden aktiv hinterfragt, auch Vorgaben des Projektverantwortlichen.

### Toolchain: llvm-mingw statt MSVC

Eine Toolchain für alle Ziele, lokal und in der CI mit denselben Versionen:

- llvm-mingw `20260922` für PS5 und Windows
- ps5-payload-sdk `v0.44`
- CMake und Ninja

**Warum:** Auf dem Entwicklungsrechner gibt es weder WSL noch Admin-Rechte. Visual Studio Community wäre für Open Source zwar kostenlos, wird so aber nicht gebraucht. Die Compiler-Wrapper des SDK hängen unter Windows `crt1.o` doppelt an. Darum wird `clang` direkt aufgerufen.

## Ergebnisse der Spikes, 20.10.2026

### S1: Port 445 ist frei

Das Payload belegt 445 mit effektiven Root-Rechten, und Windows erreicht den Port über das WLAN. Ein Alternativport entfällt.

### S2: libsmb2 bedient Windows korrekt

Lesend ist alles korrekt angekommen:

- NTLMv2 mit Signing funktioniert unter Windows 11 Pro 25H2.
- Alle Prüfsummen stimmen.
- Der Explorer zeigt keine Auffälligkeiten.
- Das fehlende SPNEGO-mechListMIC stört Windows nicht, nur Sambas `smbclient`.

### S2b: TCP_NODELAY und große Socket-Puffer werden Standard

- Mit 1 MiB Socket-Puffern und TCP_NODELAY (Profil b) gehen kleine Dateien 2,5-mal schneller.
- Die SMB-3-Signatur (AES-CMAC) bremst im WLAN noch nicht. Für G2 wird sie beschleunigt.
- SMB 3.1.1 bleibt.
- Die interne UTC-Uhr der PS5 geht 1 Stunde vor. Das ist für Zeitstempel in S3 wichtig.

### S3: Schreiben funktioniert

Bestanden im zweiten Lauf (E3, Build `46a55e4`):

- Alle Bytes, alle Änderungszeiten und alle 13 Dateivorgänge stimmen. Dazu gehören das Ändern nur der Groß-/Kleinschreibung, das Verschieben und das rekursive Löschen.
- Schreiben ist so schnell wie mit ftpsrv (ein Lauf, je 64 MiB): SMB 7,7 MB/s, FTP 7,2 MB/s.
- Neue Dateien gehören root und haben die Rechte `0666`.
- Neue Dateien ohne Zeitangabe bekommen die Zeit der PS5 und liegen damit 1 Stunde in der Zukunft. Der Kernel meldet dazu Sommerzeit von 60 Minuten.
- **Geklärt:** Die Referenzzeit kommt vom PC, siehe [Uhrabweichung](#uhrabweichung-der-pc-liefert-die-referenzzeit).

### Regel: nur libkernel-Funktionen auf der PS5

Das SDK enthält in `libc.a` für jede FreeBSD-Systemaufrufnummer einen nackten Aufruf. Diese Aufrufe setzen `errno` nicht, und auf der PS5 kann eine Nummer etwas anderes bedeuten. In S3 hat `utimensat()` deshalb nichts getan.

- **Regel:** Auf der PS5 nur Funktionen benutzen, die libkernel exportiert. Die CI prüft das (`.github/scripts/check-raw-syscalls.sh`).
- **Ebenso:** Neue Bibliotheksfunktionen gelten erst als sicher, wenn sie auf der PS5 gelaufen sind. Nach `localtime_r()` blieb das Payload stumm.

## Testablauf, 20.10.2026

### Test mit einem Klick

Der Projektverantwortliche startet nur `tools\Run-Test.cmd` und schickt die Ausgabe. Das Skript läuft so ab:

1. Es holt den Stand und den geprüften Build. Der Build muss genau zu diesem Stand passen.
2. Es startet das Payload neu, über den ELF-Loader auf Port 9021 oder von Hand im Payload-Manager.
3. Es holt das Passwort einmalig über den Steuerport.
4. Es führt den Test aus und legt die Ausgabe in die Zwischenablage.

- **Abwägung:** Das Testpayload gibt sein Passwort genau einmal heraus, danach nie wieder. Ein anderes Gerät im WLAN könnte es zuerst abholen. Es käme dann nur an den neu angelegten Testordner mit erzeugten Testdaten, für höchstens 15 Minuten (S2, nur lesen) oder 30 Minuten (S3, auch schreiben). Der Steuerport war schon vorher ohne Passwort erreichbar. Ins Produkt kommt das nicht.
- **Verworfen:** den Payload-Manager (PLDMGR) über seine Schnittstelle fernzusteuern. Das hätte in seinen bestehenden Ordner auf der PS5 geschrieben, und Tests dürfen nur in neu angelegten Testordnern schreiben. Außerdem ist die Schnittstelle fremd und inoffiziell.

## Uhrzeit, 20.10.2026

### Uhrabweichung: der PC liefert die Referenzzeit

Die interne UTC-Uhr der PS5 geht 1 Stunde vor (S2b, S3). Entschieden ist Variante 1:

- Das Windows-Programm schickt in jeder UDP-Liveness-Meldung die UTC-Zeit des PCs mit.
- Der Server misst daraus die Abweichung seiner Uhr und rechnet alle Zeitstempel um. Beim Melden an Windows zieht er die Abweichung ab, beim Setzen durch Windows zählt er sie dazu. Auf dem Datenträger stehen die Zeiten so, wie die PS5 sie selbst schreibt.
- Das Programm bleibt generisch, denn eine Zeitangabe in einer Liveness-Meldung ist nichts PS5-Spezifisches.
- **Warum:** Es funktioniert ohne Internet, und viele PS5 mit Jailbreak sind offline. Die Uhr eines Windows-PCs stellt normalerweise der Zeitdienst.
- **Verworfen:**
  - **Sommerzeit-Angabe des Kernels (`tz_dsttime`):** Ihre Bedeutung auf der PS5 ist nicht belegt.
  - **NTP auf der PS5:** braucht Internet.
- **Offen für Phase 2:**
  - Die Abweichung runden und nur bei deutlicher Änderung übernehmen, damit Zeiten nicht um Sekunden springen.
  - Nur echten Meldungen glauben, etwa nur von einem PC mit angemeldeter SMB-Sitzung oder mit einer Prüfsumme über das Passwort. Sonst könnte jedes Gerät im WLAN alle Zeiten verschieben.
  - Festlegen, was vor der ersten Meldung gilt, etwa der zuletzt gemessene Wert.
- **Zu prüfen ab 26.10.2026:** Ist die Abweichung nach dem Ende der Sommerzeit verschwunden, steckt die Sommerzeit in der Kernel-Uhr. Dann verschieben sich zweimal im Jahr die Zeiten aller älteren Dateien um 1 Stunde, wie auf der PS5 selbst. Sync-Programme würden danach einmal alles neu kopieren. Ob wir das abfangen, wird nach dieser Messung entschieden.
