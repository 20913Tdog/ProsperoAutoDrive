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
| S6 | Ist er in der CI mit echtem Windows-Client testbar? | bestanden (E2) |
| S7 | Klappt die Discovery ohne Firewall-Dialog? | bestanden (E3) |
| G1 | Go für natives SMB? | **Go** (21.10.2026) |
| **Phase 2** | PS5-Server `prospero-smbd.elf` | **beginnt** |
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
- **Ergebnis (E2, 21.10.2026):** bestanden. Der Server läuft als statischer Linux-Build in WSL 1 und hört auf Port 4450, der Client verbindet sich per `/TCPPORT`. `S3-Check.cmd` besteht dort komplett. smbtorture besteht 13 von 43 Teiltests, diese Liste darf nicht schlechter werden. `dev-build` hängt jetzt an beiden Jobs. Details: [`tests/spikes/README.md`](../tests/spikes/README.md#s6--lässt-sich-der-server-in-der-ci-mit-echten-clients-testen)

### S7 – Discovery ohne Firewall-Dialog

- Ein kleines Testprogramm für Windows schickt als Standardbenutzer eine Broadcast-Anfrage und einen Unicast-Ping. Das Payload antwortet mit seiner Server-ID. Das ist der erste Windows-Code, gebaut über das Preset `windows`.
- **Bestanden:** Die Antwort kommt an, und es erscheint kein Firewall-Dialog.
- **Neu durch Variante 1:** Die Anfrage enthält die UTC-Zeit des PCs, und das Payload meldet die gemessene Abweichung. Damit ist der Uhrausgleich schon in S7 auf E3 belegt.

### Gate G1

Go für natives SMB, wenn S1, S2, S3, S6 und S7 bestanden sind und S4 mindestens 70 % erreicht. Sonst wird der Rückfallweg dokumentiert entschieden: ein eigenes Protokoll mit eigenem WinFsp-Client. Das Dateisystem-Backend wäre wiederverwendbar.

## Phase 2 – PS5-Server

**Stand:**

- **P2.1** Grundgerüst: bestanden (E1, E2).
- **P2.2** Suche, Kopplung, Lebenszeichen: bestanden (E3, Build `8e56644`, 22.10.2026). Unicast und Broadcast antworten, der PC koppelt beim ersten Versuch, die Zugangsdaten liegen in der Anmeldeinformationsverwaltung. Alle drei `PING` mit Prüfsumme wurden anerkannt, Abweichung 3596,3 s. Die Testdatei in `/data/_ProsperoAutoDrive_Test_…` wurde geschrieben und gleich zurückgelesen. Kein Firewall-Fenster. Die Benachrichtigungen wurden nicht beobachtet, „läuft“ wird beim Test zu P2.3 mitgeprüft.
- **P2.3** Uhrausgleich: bestanden (E3, Build `aed080a`, 22.10.2026). Nach dem Neustart galt sofort der gespeicherte Wert 3596 s, die neue Testdatei wich 0 s von der PC-Uhr ab. Die Benachrichtigung „läuft“ kam. Mit `tools\Connect-Share.cmd` öffnet der Explorer die Freigabe ohne Passwortabfrage, die Testordner zeigen die richtige Uhrzeit (E3).
- **P2.4** Antwortzeit unter Last, Messung 1 (E3, Build `a35fdce`, 22.10.2026, 2 Runden je 256 MiB):
  - Kleine Datei ohne Last 63 ms. Während des Schreibens Median 1,8–1,9 s, während des Lesens 3,0–3,2 s (max 5,9 s).
  - Der Dienstkanal auf eigener TCP-Verbindung blieb schnell: Median 16 ms beim Schreiben, 62 ms beim Lesen. Das WLAN selbst staut also kaum.
  - Lesen: Im Server warten höchstens etwa 1 MiB, je zur Hälfte in der Sendeschlange von libsmb2 und im Sendepuffer des Sockets. Der ist 512 KiB groß, nicht 1 MiB wie angefordert. Bei 3,9 MB/s sind das etwa 0,27 s je Anfrage. Eine kleine Datei braucht mehrere SMB-Anfragen nacheinander, jede wartet erneut.
  - Schreiben: Im Server wartet nichts. Die Wartezeit entsteht vor dem Server, beim PC oder auf dem Weg dorthin.
  - Durchsatz wie in S4: Schreiben 5,3–5,5 MB/s, Lesen 3,9 MB/s.
  - Noch nicht gemessen: ungelesene Anfragen im Empfangspuffer der PS5, Anfragen je kleiner Datei, Kredite des PCs.
  - Einschränkung: Messung 1 kopierte mit `ReadFile`/`WriteFile` zu je 1 MiB, also mit nur einer Anfrage zur Zeit. Der Explorer hat mehrere offen.
- **P2.4** Messung 2 (E3, Build `e2516f4`, 22.10.2026, 5 Varianten × 2 Runden je 128 MiB, Kopie mit `CopyFileEx` wie der Explorer):

  | Variante | Schreiben MB/s | klein ms | Lesen MB/s | klein ms |
  |---|---|---|---|---|
  | Puffer Standard (512 KiB), Kredite frei | 8,69 | 7969 | 4,69 | 24000 |
  | Puffer 128 KiB, Kredite frei | 8,96 | 9766 | 3,68 | 32062 |
  | Puffer 64 KiB, Kredite frei | 9,13 | 12140 | 2,96 | 41828 |
  | Puffer 128 KiB, Kredite 64 | 9,09 | 4078 | 3,32 | 23562 |
  | Puffer 128 KiB, Kredite 32 | 7,60 | 3047 | 2,65 | 26875 |

  - Einschränkung: Das Testprogramm kopiert mit `CopyFileEx` ohne Puffer. Dass der Explorer große Dateien genauso kopiert, ist angenommen, nicht gemessen. Gegenprobe mit dem echten Explorer und mit einem anderen SMB-Server im selben WLAN steht aus.
  - ftpsrv im selben Lauf: Schreiben 8,3 und 9,0 MB/s, Lesen 4,7 und 2,9 MB/s. SMB liegt damit beim Durchsatz gleichauf. Das Lesen schwankt im WLAN bei beiden Verfahren stark.
  - Wie der Explorer hält der PC bis zu 11 Lese-Antworten zu je 1 MiB offen, bis 8 MiB stehen in der Sendeschlange von libsmb2. Eine kleine Datei wartet daher beim Lesen 24–44 s und beim Schreiben 8–12 s, ohne Last 78 ms. Kleinere Sendepuffer helfen nicht, die Antworten stauen sich dann in der Sendeschlange.
  - Die Kreditgrenze verkürzt die Wartezeit beim Schreiben auf 3–4 s, beim Lesen kaum. Den Durchsatz kostet sie bei 64 nicht.
  - Messung 2 war WLAN.
  - Fehler: In Runde 1 brach das Schreiben mit Kreditgrenze 64 nach 7 s ab (Windows-Fehler 59, der PC hat die Verbindung zurückgesetzt). Davor lief eine Variante ohne Grenze, der PC hielt bis zu 222 Kredite. Mit derselben Grenze in Runde 2 trat der Fehler nicht auf. Vermutung: Die Grenze wurde mitten in der Verbindung von 222 auf 64 gesenkt. Noch nicht nachgewiesen.
- **P2.4** Messung 3, **Kabel** (E3, Build `379f98c`, 23.10.2026, dieselben Varianten, 128 MiB):
  - Durchsatz: SMB liest 26–28 MB/s, schreibt 25 MB/s. ftpsrv liest 114 MB/s, schreibt 89 MB/s. Das sind etwa 24 % und 28 %.
  - Ursache: Rechenzeit. Beim Lesen rechnete der Server 4,1 s in 4,7 s. libsmb2 signiert mit einer Referenz-AES, die je 16-Byte-Block den Schlüssel neu berechnet. Lokal gemessen: 75 MB/s für AES-CMAC mit dieser Referenz, 1942 MB/s mit AES-NI, gleiche Ergebnisse und alle Prüfwerte aus RFC 4493 richtig.
  - Wartezeit: kleine Datei ohne Last 16 ms, unter Last 2,7–3,4 s, mit Kreditgrenze 32 beim Schreiben etwa 1 s. Das passt zu 8 MiB Schlange bei 28 MB/s.
  - Fehler 59 wieder genau in Runde 1, Variante 4, beim ersten Absenken der Kreditgrenze mitten in der Verbindung. Der Patch vergibt jetzt nie 0 Kredite, wenn der PC welche anfordert (MS-SMB2 3.3.1.2).
- **P2.4** Messung 4, **Kabel** (E3, Build `a4c9e7b`, 23.10.2026, 1 GiB; die Ausgabe kam nur bis Runde 1, Variante 2 an):
  - Signatur auf der PS5: AES-NI aktiv, 1189 MB/s, Referenz 36 MB/s.
  - Standard: Lesen 115 MB/s (ftpsrv in Messung 3: 114 MB/s), Schreiben 62 MB/s (ftpsrv: 89 MB/s, also etwa 70 %). Kleine Datei unter Last etwa 1 s.
  - Mit Kreditgrenze 32 (per `TUNE` mitten in der Verbindung) brauchte das Schreiben 63 s, danach hing das Lesen über 18 min ohne Fortschritt, während kleine Dateien auf derselben Verbindung in 16 ms gingen. Vermutlich wartete Windows auf genug Kredite für eine große Leseanfrage. Kreditgrenzen werden deshalb nicht weiter gemessen, P24 misst nur noch die Voreinstellung. Das Testprogramm bricht eine Kopie nach 60 s ohne Fortschritt ab.
  - Offen: warum das Schreiben bei 70 % bleibt. Das Testprogramm zeigt dafür jetzt je Phase die Rechenzeit des Servers und die Zeit in `pread`/`pwrite`.
- **P2.4** Messung 5, **Kabel** (E3, Build `80f38a9`, 23.10.2026, 3 × 1 GiB, nur Voreinstellung):

  | | Runde 1 | 2 | 3 | Median | ftpsrv (3 Läufe) | Anteil |
  |---|---|---|---|---|---|---|
  | Schreiben MB/s | 57,2 | 80,1 | 78,7 | 78,7 | 89,5–90,0 | 88 % |
  | Lesen MB/s | 89,7 | 79,5 | 103,5 | 89,7 | 115,6–117,1 | 77 % |

  - Schreiben: Der Server verbrachte 6,5 s von 13,4 s je GiB in `pwrite` (etwa 6 ms je MiB), Rechenzeit 37 %. Empfangen und Schreiben liefen nacheinander.
  - Lesen: Rechenzeit nur 22–29 %, `pread` 1,4 s je GiB. Die Sendeschlange war voll (7 MiB), der Socket aber zeitweise leer: libsmb2 schreibt neue Antworten erst beim nächsten Durchlauf in den Socket, wenn schon welche warten.
  - Kleine Datei unter Last: beim Schreiben etwa 1 s, beim Lesen 0,05–0,9 s.
  - Maßnahmen: `pwrite` großer Schreibanfragen in einem eigenen Thread, die Antwort geht weiter erst nach dem Schreiben raus (eine Anfrage zur Zeit, alle anderen Anfragen warten auf sie). Patch `libsmb2-server-flush`: Der Server schreibt jede neue Antwort sofort in den Socket.
- **P2.4** Messung 6, **Kabel** (E3, Build `43aa8f1`, 24.10.2026, 3 × 1 GiB):

  | | Runde 1 | 2 | 3 | Median | ftpsrv (3 Läufe) | Anteil |
  |---|---|---|---|---|---|---|
  | Schreiben MB/s | 65,7 | 76,1 | 77,8 | 76,1 | 73,8 | 103 % |
  | Lesen MB/s | 111,9 | 107,6 | 107,5 | 107,5 | 114,1–114,9 | 94 % |

  - ftpsrv schrieb an diesem Tag nur 74 MB/s (Messung 5: 89 MB/s), `pwrite` brauchte 8,2 s je GiB statt 6,5 s. Die SSD der PS5 war langsamer.
  - Schreiben: Der Schreib-Thread übernahm 1024 von 1028 Anfragen, aber die Hauptschleife wartete 7,5 s von 8,2 s auf ihn. Ursache: Sie wartete, sobald gerade keine Daten lesbar waren, also auch mitten im Empfang der nächsten Anfrage. Jetzt wartet sie dort nie, der Thread weckt sie über eine Pipe.
  - Lesen: von 77 % auf 94 %, die Sendeschlange und der Socket waren durchgehend voll.
- **P2.4** Messung 7, **Kabel** (E3, Build `79bf749`, 24.10.2026, 3 × 1 GiB):

  | | Runde 1 | 2 | 3 | Median | ftpsrv (3 Läufe) | Anteil |
  |---|---|---|---|---|---|---|
  | Schreiben MB/s | 88,9 | 96,9 | 97,6 | 96,9 | 73,4–73,9 | 131 % |
  | Lesen MB/s | 110,7 | 106,7 | 106,4 | 106,7 | 112,3–113,1 | 95,0 % (genau 94,98 %) |

  - Schreiben: Die Wartezeit auf den Schreib-Thread sank von 7,5 s auf 1,6 s je GiB. Jetzt begrenzt `pwrite` selbst (8,7 s je GiB, fast nur Systemzeit), die Rechenzeit liegt bei 83–92 %.
  - Lesen: unverändert gegenüber Messung 6. Sendeschlange und Socket voll, Rechenzeit 50 %, der Server wartet also nicht. In Messung 6 und 7 war Runde 1 je etwa 4 % schneller als Runde 2 und 3.
  - Bewertung: Lesen liegt genau auf der Grenze. Mit 3 Runden ist das nicht von 94 % oder 96 % zu unterscheiden, und ftpsrv lief erst nach allen SMB-Runden. P24 misst deshalb jetzt wie für G2 vorgesehen: 7 Runden, SMB und ftpsrv im Wechsel, ftpsrv liest die Datei der jeweiligen Runde.
- **P2.4** Messung 8, **Kabel** (E3, Build `86e62c1`, 24.10.2026, 7 × 1 GiB, SMB und ftpsrv im Wechsel, wie für G2 vorgesehen):

  | Runde | 1 | 2 | 3 | 4 | 5 | 6 | 7 | Median |
  |---|---|---|---|---|---|---|---|---|
  | SMB schreiben MB/s | 86,1 | 100,0 | 100,0 | 99,9 | 99,9 | 99,9 | 100,3 | 99,9 |
  | ftpsrv schreiben MB/s | 73,5 | 74,1 | 73,9 | 73,5 | 73,9 | 73,5 | 73,7 | 73,7 |
  | SMB lesen MB/s | 107,0 | 101,5 | 103,3 | 108,4 | 108,9 | 108,7 | 108,7 | 108,4 |
  | ftpsrv lesen MB/s | 107,7 | 106,8 | 106,0 | 106,8 | 111,0 | 109,6 | 110,2 | 107,7 |

  - **Ziel am Kabel erreicht:** Schreiben 136 %, Lesen 101 % von ftpsrv (Median). Lesen je Runde 95,0–101,5 %, keine Runde unter 95 %.
  - ftpsrv las diesmal nur 106–111 MB/s (Messung 5–7: 112–117 MB/s). SMB las wie in Messung 6 und 7 (107–108 MB/s). Gegen den besten ftpsrv-Wert der Vortage wären es etwa 93 %.
  - Kleine Datei unter Last: etwa 1,0–1,2 s, ohne Last 15 ms. Das bleibt für später offen.
  - Ziel des nächsten Schritts war: ≥ 95 % von ftpsrv am Kabel, siehe [Entscheidungen](entscheidungen.md#ziele-nach-messung-2-am-kabel-23102026).

**Umfang:**

- Port 445, eine Freigabe `/`, lesen und schreiben
- Zugangsdaten: werden beim Koppeln übertragen, nie angezeigt
- Konfiguration, Ringlog, Weiterlaufen nach dem Ruhemodus, Plattformschicht
- Suche per UDP 4445 und Dienstkanal auf TCP 4445 mit Server-ID, Lebenszeichen, Kopplung (Variante C) und Uhrausgleich (Variante 1), siehe [Entwurf Phase 2](entwurf-phase2.md)
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

Entwurf: [`entwurf-phase3.md`](entwurf-phase3.md).

**Stand:**

- **P3.1** Zustandsautomat (`windows/app/core/`): Unit-Tests mit simulierter Uhr und simuliertem Server (E1, 24.10.2026, lokal). Laufwerk nach höchstens 2,1 s, egal wann die PS5 erscheint. Weg nach 4,1 s am Kabel, 13,1 s im WLAN, 2 ms bei ordentlichem Ende. Aussetzer von 2 s (Kabel) und 9 s (WLAN) trennen nicht. Falsches Passwort: eine Meldung, ein Versuch je Minute. Im Leerlauf ein Aufwachen je Sekunde (online) bzw. je 2 s (suchend).
- **P3.2** `ProsperoAutoDrive.exe` über die Kommandozeile (E2, 24.10.2026): Laufwerk nach 74 ms, als `O:`, weil ein fremdes `P:` belegt war und unberührt blieb. Ende des Servers sofort erkannt, aber das Entfernen des Laufwerks dauerte 5 s, mit `WNetCancelConnection2` und mit `NetUseDel`. In der CI hängt das Laufwerk an einer `net use`-Verbindung mit anderem Port. Ob das an 445 auch so ist, misst `P32-Check.cmd` live. Das Wiederkommen nach einem Neustart prüft nur der Live-Test, weil Windows den anderen Port der CI mit der alten Verbindung vergisst.
- **P3.2** live, **Kabel** (E3, Build `7eb440b`, 24.10.2026, `P32-Check.cmd`):
  - Start: `P:` 0,1 s nach dem Programmstart.
  - Payload neu gesendet (ordentliches Ende): Ende sofort erkannt, `P:` in 0 ms entfernt. Die 5 s aus der CI gibt es an Port 445 nicht. Wieder da 2,0 s später, sobald die neue Instanz lief.
  - Kabel gezogen: `P:` weg etwa 3–4 s danach. Der genaue Zeitpunkt des Ziehens ist nicht bekannt, weil der Test erst nach dem Ziehen Enter verlangte. Das Log nennt künftig das Alter der letzten Antwort.
  - Kabel eingesteckt: `P:` im selben Moment wie der Dienstkanal da. Wie lange die PS5 nach dem Einstecken selbst braucht, ist nicht gemessen.
  - Alle Ziele erreicht: Erscheinen ≤ 3 s, Verschwinden ≤ 5 s am Kabel, ordentliches Ende < 1 s.

- **Viele Dateien in einem Ordner** (P34, live: Löschen von 100.000 Dateien ließ den Dienstkanal minutenlang ohne Antwort). Ursache: Auflisten mit `stat()` für jeden Eintrag in einer einzigen Anfrage, und jede Suche nach einem einzelnen Namen las den ganzen Ordner. Behoben in `13d3c91`. Live mit 20.000 Dateien (E3, Build `a6b7ca7`, 25.10.2026, Kabel): Lebenszeichen höchstens 16 ms in allen Phasen; Anlegen 138 s (4,5 ms je Datei am Anfang, 9,2 ms am Ende), einzeln suchen 0,8–1 ms, Auflisten 0,45 s, Löschen 118 s (5,8 bis 4,4 ms). Offen: Anlegen wird mit der Größe des Ordners langsamer, weil jeder neue Name ohne Rücksicht auf Groß/klein gesucht wird.
- **Wieder koppeln nach Neuinstallation** ([Entscheidung](entscheidungen.md#wieder-koppeln-nach-neuinstallation-25102026)): E1 grün, live bestanden (E3, Build `11b3c37`, 25.10.2026). Neu installiert, Meldung kam, Server im Dashboard neu gestartet, „Neu koppeln“ koppelte in 20 ms, `P:` sofort da. Vorher lief versehentlich eine alte ELF-Datei aus dem Payload Manager. Nach einem neuen Build muss auch die dort eingetragene Datei ersetzt werden.
- **Server beenden** aus dem Tray (signiertes `QUIT`, der Server sagt zuletzt `GOODBYE`) und **Mit neuem Server koppeln** (nach einem Zurücksetzen des Servers): E1 und E2 grün, live bestanden (E3, Build `7f97ddc`, 25.10.2026).

**Umfang:**

- eine einzelne exe, ohne Installer-Paket und ohne ZIP
- Einrichtung beim ersten Start: PS5 finden, koppeln (Variante C), Zugangsdaten in der Windows-Anmeldeinformationsverwaltung ablegen
- Zustandsautomat: Lebenszeichen über TCP 4445 mit PC-Zeit, Trennen erst, wenn das Lebenszeichen **und** TCP 445 fehlschlagen
- temporäres Mapping `P:`; ein fremdes Mapping auf `P:` bleibt unberührt
- Tray-Symbol, Benachrichtigungen, Autostart für den angemeldeten Benutzer, Ringlog
- Deinstallation ohne Rückstände

**Tests (E1/E2):**

- Unit-Tests des Zustandsautomaten mit simulierter Uhr und simulierter Liveness
- CI-Integration:
  - Server an → `P:` ist nach höchstens 3 s da.
  - Server aus → `P:` ist nach höchstens 5 s weg (Kabel), 15 s (WLAN), unter 1 s bei ordentlichem Ende des Payloads ([Entscheidung](entscheidungen.md#sofort-für-verbinden-und-trennen-24102026)).
  - Leerlauf mit verbundenem PC: Rechenzeit unter 0,1 % eines Kerns, auf dem PC und auf dem Server gemessen.
  - Server wieder an → `P:` ist wieder da.
  - Ein fremdes Mapping bleibt unberührt.
  - Falsches Passwort → Hinweis, aber kein Sturm von Wiederholungen.
  - Server 5 s weg → kein Trennen.

**Fertig, wenn:** die CI grün ist, das Manifest `asInvoker` setzt, das Programm im Betrieb ohne Admin läuft, die Installation in einen geschützten Ordner genau einen UAC-Dialog zeigt und in einen freien keinen (live geprüft), und nach der Deinstallation nichts zurückbleibt.

**Danach:** erstes Release 0.1.0 per PR von `dev` nach `main`, nach dem Live-Test des Projektverantwortlichen.

### P33 – Payload als eigener Prozess

[Entscheidung](entscheidungen.md#payload-als-eigener-prozess-25102026): Payload Manager startet `prospero-smbd.elf` aus einer Datei als eigenen Prozess, statt es per Websend in `pldmgr.elf` zu laden. Die Datei liegt in einem eigenen Ordner, etwa `/data/prospero/` oder auf dem USB-Stick, nie in `/data/homebrew`. Alle Schritte macht der Projektverantwortliche von Hand (E3).

| Nr. | Schritt | Erwartet |
|---|---|---|
| P33.1 | In „Manage Payloads“ eintragen und im Dashboard starten | eigener Prozess unter „Active Processes“, nicht unter `pldmgr.elf`; `P:` nach ≤ 3 s |
| P33.2 | Über „Active Processes“ beenden | `P:` weg nach ≤ 5 s am Kabel |
| P33.3 | Erneut starten | `P:` nach ≤ 3 s wieder da, ohne neue Kopplung |
| P33.4 | Unter „Active Processes“ ansehen; pldmgr lässt sich nicht beenden, weil es zum Jailbreak gehört | eigener Eintrag mit eigener PID neben `pldmgr.elf`; beim Beenden in P33.2 läuft pldmgr weiter |
| P33.5 | In „Autoload“ ans Ende setzen, PS5 neu starten | `P:` kommt ohne Zutun |
| P33.6 | Neue ELF-Datei über `P:` auf die alte kopieren, Server über „Active Processes“ beenden und aus dem Dashboard neu starten | Kopieren klappt, obwohl der Server läuft; danach läuft die neue Version |
| P33.7 | Ein zweites Mal starten, per Autoload und per Websend | Die neue Instanz beendet die alte, wie schon bei Websend; danach läuft genau ein Server, und `P:` kommt nach ≤ 3 s wieder |

**Kurzprobe** (E3, Build `bcaa478`, 25.10.2026, Kabel):

- P33.1 teilweise: Im Dashboard steht „prospero smbd“ mit grünem Haken, ohne Version, und `P:` ist verbunden. Unter „Active Processes“ steht der Server aber **nicht**. Ob er ein eigener Prozess ist, ist damit offen.
- P33.6 bestanden: Die Datei ließ sich über `P:` überschreiben, während der Server lief. Neustart aus dem Dashboard: Die neue Instanz beendete die alte, das Programm meldete das Trennen und verband sofort neu.

**Stand:** Alles Weitere wird per Websend entwickelt. Alles Weitere wird per Websend entwickelt. Der vollständige Test P33 läuft vor dem Release.

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

- [ ] Installation ohne Fremdsoftware in höchstens 2 Minuten; Pfad frei wählbar (Standard `C:\Program Files\ProsperoAutoDrive\`), höchstens ein UAC-Dialog; im Betrieb nie Admin
- [ ] `P:` erscheint höchstens 3 s nach Erreichbarkeit und verschwindet höchstens 5 s (Kabel) bzw. 15 s (WLAN) nach Wegfall (Median über mindestens 10 Zyklen, kein Ausreißer über 30 s)
- [ ] Im Leerlauf weniger als 0,1 % eines Kerns Rechenzeit, auf PC und PS5
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
