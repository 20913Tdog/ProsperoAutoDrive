# Fahrplan

Stand: 20.10.2026. Der ursprüngliche Plan lag nur lokal und wurde gelöscht. Dieser Fahrplan ist aus dem Arbeitsverlauf rekonstruiert und an die späteren Entscheidungen angepasst ([`entscheidungen.md`](entscheidungen.md)). Aus „Release 2.0.0“ wurde 1.0.0 (P6), die Migration von 1.x entfällt, das Programm hat immer ein Tray-Symbol (D4) und ist in C++/Win32 geschrieben (D5).

Evidenzebenen: **E1** Unit unter Linux, **E2** Protokoll-CI mit echtem Windows-Client, **E3** echte PS5.

## Überblick

| Schritt | Frage | Stand |
|---|---|---|
| S1 | Ist Port 445 auf der PS5 frei? | bestanden (E3) |
| S2, S2b | Bedient libsmb2 Windows korrekt, und wie schnell? | bestanden (E3) |
| S3 | Funktioniert Schreiben? | bestanden (E3) |
| S4 | Reicht der Durchsatz? | bestanden (E3): Lesen 96 %, Schreiben 98 % von ftpsrv |
| S5 | Übersteht der Server Ruhemodus und Neustart? | entfällt als Live-Test, wird in der CI simuliert |
| **S6** | Ist er in der CI mit echtem Windows-Client testbar? | **in Arbeit** |
| S7 | Klappt die Discovery ohne Firewall-Dialog? | offen |
| **G1** | Go für natives SMB? | nach S6 und S7 |
| Phase 2 | PS5-Server `prospero-smbd.elf` | |
| Phase 3 | Windows-Programm `ProsperoAutoDrive.exe` | erstes Release 0.1.0 |
| Phase 4 | Härtung | weitere 0.x |
| Phase 5 | Live-Abnahme, **G2** | Release 1.0.0 |

Vor G1 entsteht kein Produktcode.

## Spikes

### S4 – Durchsatz

- Eine große Datei lesen und schreiben, abwechselnd SMB und ftpsrv (Port 2121) im selben WLAN. Je Richtung mindestens 7 Läufe, entscheidend ist der Median.
- Dazu:
  - die Signatur mit HMAC-SHA256 (SMB 2.1) gegen AES-CMAC (SMB 3.1.1)
  - die CPU-Zeit auf der PS5
  - die Antwortzeit beim Lesen einer kleinen Datei während einer großen Kopie, weil libsmb2 Anfragen nacheinander bearbeitet (Risiko R4)
- **Bestanden:** Der Median von SMB erreicht in jeder Richtung mindestens 70 % von ftpsrv (G1).
- **Dateigröße:** 256 MiB je Lauf, entschieden am 20.10.2026. Der alte Plan sah 2 GiB vor. Im WLAN mit 5–8 MB/s hätte ein Lauf damit etwa 5 Minuten gedauert, alle 28 Läufe über 2 Stunden.
- Ablauf und Auswertung: [`tests/spikes/README.md`](../tests/spikes/README.md#s4--reicht-der-durchsatz)
- **Ergebnis:** bestanden. Lesen 96 %, Schreiben 98 % von ftpsrv. Beim Lesen streuen die Runden stark (70–119 %). Die Wartezeit für eine kleine Datei steigt unter Last von 44 ms auf 2,9 s. Das wird in Phase 2 erst gemessen, dann behoben.

### S5 – Ruhemodus und Neustart

**Entfällt als Live-Test (Entscheidung vom 21.10.2026).** In der CI simulieren Skripte, dass der Server wegfällt und wiederkommt. Ob die PS5 nach dem Ruhemodus die Verbindungen des Payloads erhält und ob das Payload nach einem Neustart von selbst startet, zeigt erst der normale Betrieb in Phase 5. Dafür richtet der Projektverantwortliche selbst einen Autostart ein, etwa über das Autoload von PLDMGR oder etaHEN.

### S6 – CI mit echtem Windows-Client

- **Windows-Runner:** der echte SMB-Client von Windows gegen den echten Servercode, als Smoke-Test.
- **Linux-Runner:** `mount -t cifs` und eine Teilmenge von smbtorture.
- **Bestanden:** Beide Jobs sind grün auf dem veröffentlichten Commit.
- **Zu klären:**
  - Wie der Server auf dem Windows-Runner läuft, ohne MSVC: als Windows-Build mit llvm-mingw, was eine Windows-Schicht für das POSIX-Backend braucht, oder als Linux-Build in WSL 1.
  - Welcher Port: Auf dem Runner belegt Windows selbst Port 445. Möglich sind der Alternativport 4445 (Client ab Windows 11 24H2 bzw. Server 2025) oder das Abschalten des Windows-Serverdienstes auf dem Runner.

### S7 – Discovery ohne Firewall-Dialog

- Ein kleines Testprogramm für Windows schickt als Standardbenutzer eine Broadcast-Anfrage und einen Unicast-Ping. Das Payload antwortet mit seiner Server-ID. Das ist der erste Windows-Code, gebaut über das Preset `windows`.
- **Bestanden:** Die Antwort kommt an, und es erscheint kein Firewall-Dialog.
- **Neu durch Variante 1:** Die Anfrage enthält die UTC-Zeit des PCs, und das Payload meldet die gemessene Abweichung. Damit ist der Uhrausgleich schon in S7 auf E3 belegt.

### Gate G1

Go für natives SMB, wenn S1, S2, S3, S6 und S7 bestanden sind und S4 mindestens 70 % erreicht. Sonst wird der Rückfallweg dokumentiert entschieden: ein eigenes Protokoll mit eigenem WinFsp-Client. Das Dateisystem-Backend wäre wiederverwendbar.

## Phase 2 – PS5-Server

**Umfang:**

- Port 445, eine Freigabe `/`, lesen und schreiben
- Zugangsdaten mit Anzeige als PS5-Benachrichtigung
- Konfiguration, Ringlog, Weiterlaufen nach dem Ruhemodus, Plattformschicht
- UDP-Liveness mit Server-ID und Uhrausgleich (Variante 1)
- **Antwortzeit unter Last:** S4 maß 2,9 s für eine kleine Datei während einer großen Kopie.
  - Zuerst messen, welcher Puffer wie viel davon ausmacht: die Sendeschlange von libsmb2 (jeder Kredit wird gewährt, Antworten stehen hinten an), der Sendepuffer von 1 MiB oder WLAN und Router.
  - Dann gezielt beheben: Kredite begrenzen, kleine Antworten vorziehen, Sendepuffer verkleinern. `S4-Check.cmd` misst die Wirkung.
- offene Punkte aus S3:
  - **Sicherheitsbeschreibung** (QUERY_INFO Typ 3): Der Explorer fragt sie oft ab, in S3 über 3000-mal. Sie gehört zur Registerkarte „Sicherheit“.
  - **Verzeichnisklasse 0x51:** Windows weicht aus, das kostet je Ordner eine zusätzliche Anfrage.
  - **Freigabemodi und Sperren** durchsetzen, wichtig für Office und gleichzeitige Zugriffe
  - **Dateiattribute** wie „Schreibgeschützt“ und „Versteckt“, bisher ignoriert
  - **Rechte neuer Dateien** (root, `0666`): prüfen, ob Programme auf der PS5 damit zurechtkommen
  - **IPC$** für die Freigabenliste unter `\\PS5`, optional, `P:` braucht sie nicht

**Tests (E1/E2):**

- Unit-Tests des Backends unter Linux
- smbtorture-Suites, unter anderem `smb2.connect`, `smb2.read`, `smb2.dir`, `smb2.rename`, `smb2.setinfo`, `smb2.getinfo`, `smb2.delete-on-close-perms`
- Linux-cifs-Mount mit `cache=none` und **fsx**, einem Zufallstester für Seek, Truncate und gemischtes Lesen und Schreiben
- **Differential-Tester** auf dem Windows-Runner: dieselbe zufällige Folge von Operationen auf einer lokalen NTFS-Datei und auf dem Laufwerk, Vergleich nach jedem Schritt
- Operationen wie im Explorer: CopyFile, MoveFileEx, CreateDirectory, SetEndOfFile, Überschreiben
- 7-Zip-Entpacken mit SHA-256
- Fuzzing des Request-Parsers, nächtlich

**Fertig, wenn:** alle Suites auf genau dem veröffentlichten Commit grün sind, fsx 100.000 und der Differential-Tester 10.000 Operationen ohne Abweichung schaffen und das Fuzzing mindestens 1 Stunde ohne Absturz läuft.

## Phase 3 – Windows-Programm

**Umfang:**

- eine einzelne exe, ohne Installer-Paket und ohne ZIP
- Einrichtung beim ersten Start: PS5 finden (S7), Passwort eingeben und in der Windows-Anmeldeinformationsverwaltung ablegen
- Zustandsautomat: UDP-Liveness mit PC-Zeit, Trennen erst, wenn UDP **und** TCP 445 fehlschlagen
- temporäres Mapping `P:`; ein fremdes Mapping auf `P:` bleibt unberührt
- Tray-Symbol, Benachrichtigungen, Autostart für den angemeldeten Benutzer, Ringlog
- Deinstallation ohne Rückstände

**Tests (E1/E2):**

- Unit-Tests des Zustandsautomaten mit simulierter Uhr und simulierter Liveness
- CI-Integration:
  - Server an → `P:` ist nach höchstens 10 s da.
  - Server aus → `P:` ist nach höchstens 15 s weg.
  - Server wieder an → `P:` ist wieder da.
  - Ein fremdes Mapping bleibt unberührt.
  - Falsches Passwort → Hinweis, aber kein Sturm von Wiederholungen.
  - Server 5 s weg → kein Trennen.

**Fertig, wenn:** die CI grün ist, das Manifest `asInvoker` setzt, Installation und Deinstallation keinen UAC-Dialog zeigen (live geprüft) und nach der Deinstallation nichts zurückbleibt.

**Danach:** erstes Release 0.1.0 per PR von `dev` nach `main`, nach dem Live-Test des Projektverantwortlichen.

## Phase 4 – Härtung

- **CHANGE_NOTIFY:** Der Explorer aktualisiert sich selbst.
- **Ereignisse:** PC-Standby, WLAN-Wechsel, IP-Wechsel
- **Worker-Threads** für READ/WRITE, nur falls die Maßnahmen aus Phase 2 die Antwortzeit unter Last nicht genug senken
- **Serverseitiges Kopieren**, in S3 als nicht unterstützter IOCTL gesehen: Kopieren innerhalb von `P:` läuft sonst zweimal durchs WLAN.
- **Schnellere AES-CMAC-Signatur** für Kabelnetze. S4 hat etwa 20 ms CPU je MiB mehr als mit HMAC gemessen. Im WLAN bremst sie nicht.
- **Leases**, optional, für schnelleres Lesen. Dann muss erneut nachgewiesen werden, dass ein Kopierabbruch sichtbar bleibt und nichts nachgesendet wird.
- **72-Stunden-Dauertest** in der CI mit zufälligen Server-Neustarts
- **Signierung prüfen:** SignPath Foundation oder Microsoft Store, beide kostenlos

## Phase 5 – Live-Abnahme (E3) und Release 1.0.0

**Wird gekürzt (20.10.2026):** Der Projektverantwortliche hält den Umfang unten für zu groß. Eine schlankere Fassung wird noch abgestimmt.

Alle Schreibtests laufen nur im neu angelegten Testordner:

1. Explorer-Operationsmatrix komplett
2. 7-Zip-Entpacken direkt auf `P:` mit SHA-256 aller Dateien
3. Differential-Tester mit 10.000 Operationen
4. 10-GiB-Kopie hin und zurück, SHA-256 identisch
5. 10× PS5 aus und an, 3× Ruhemodus, 1× IP-Wechsel, 2× PC-Standby, mit Zeiten aus dem Log des Programms
6. 7 Tage Betrieb ohne jeden Eingriff, belegt durch das Log

**Gate G2:** alle Punkte mit Zeitstempel, Commit und bereinigtem Bericht belegt, dazu ein Durchsatz von mindestens 95 % von ftpsrv, gemessen wie in S4. Dann PR nach `main` und Release 1.0.0.

## Abnahmekriterien 1.0.0

- [ ] Installation als Standardbenutzer ohne UAC-Dialog und ohne Fremdsoftware, in höchstens 2 Minuten
- [ ] `P:` erscheint höchstens 10 s nach Erreichbarkeit und verschwindet höchstens 15 s nach Wegfall (Median über mindestens 10 Zyklen, kein Ausreißer über 30 s)
- [ ] Keine Wiederherstellungsmeldung beim Anmelden, kein toter Laufwerksbuchstabe, kein fremdes Laufwerk berührt
- [ ] Explorer-Matrix, 7-Zip, Truncate/Seek und Differential-Tester zu 100 % bestanden
- [ ] 10-GiB-Kopie hin und zurück mit identischem SHA-256; ein Kopierabbruch zeigt einen Fehler, nichts wird nachgesendet
- [ ] 7 Tage Betrieb mit mindestens 10 PS5-Zyklen ohne Eingriff
- [ ] Durchsatz mindestens 95 % von ftpsrv (G2)
- [ ] Alle Tests nur im neu angelegten Testordner; `/data/homebrew` unberührt
- [ ] CI grün auf genau dem freigegebenen Commit; Binaries mit Hash und Quellstand
- [ ] Dokumentation: Einrichtung, bekannte Einschränkungen, Deinstallation

## Übergreifend offen

- **Uhr:** Messung ab 26.10.2026, ob die Abweichung mit dem Ende der Sommerzeit verschwindet ([Entscheidung](entscheidungen.md#uhrabweichung-der-pc-liefert-die-referenzzeit))
- **libsmb2:** gefundene Fehler upstream melden, nur nach Freigabe des Projektverantwortlichen
- **Windows 10 22H2:** Best Effort, vor 1.0.0 mindestens einmal live prüfen
- **Risiken:**
  - Microsoft schaltet NTLM langfristig ab. Das Backend ist protokollneutral, und libsmb2 kann Kerberos.
  - Firmenrichtlinien auf verwalteten PCs. Die Zielgruppe sind private PCs, Fehler werden klar gemeldet.
  - Änderungen an Firmware oder Exploit-Kette. Es werden nur Standardaufrufe des SDK verwendet.
  - Absturz des Payloads. Dagegen helfen Fuzzing, eine äußere Neustartschleife und das Log.
