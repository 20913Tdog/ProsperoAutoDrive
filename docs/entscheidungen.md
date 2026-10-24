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

### S4: Der Durchsatz reicht, G1 bestanden

Bestanden (E3, Build `299cb3d`, WLAN, 7 abwechselnde Läufe je Richtung mit 256 MiB, Median):

- Lesen: SMB 3,90 MB/s gegen FTP 4,06 MB/s, also **96 %**. Je Runde verglichen streut das Verhältnis von 70 bis 119 %, Median 100 %.
- Schreiben: SMB 7,10 MB/s gegen FTP 7,20 MB/s, also **98 %**. Je Runde 96 bis 101 %.
- Beim Schreiben ist SMB gleichauf mit FTP. Beim Lesen im Median ebenfalls, aber mit 7 Runden nur auf etwa ±8 % genau.
- **SMB 3.1.1 bleibt.** Es erreicht ohnehin fast die FTP-Werte. Der Vergleich mit SMB 2.1 (HMAC) lief nur 3-mal und ohne FTP-Lauf daneben.
- **CPU:** AES-CMAC kostet etwa 20 ms je MiB mehr als HMAC-SHA256. Der ganze Server braucht 36 bis 39 ms je MiB in einem Thread. Er kommt damit höchstens auf etwa 26 bis 27 MiB/s. Im WLAN spielt das keine Rolle, im Kabelnetz schon. Die schnelle AES-CMAC bleibt für Phase 4 vorgemerkt.
- **Dateigröße:** 256 MiB statt 2 GiB je Lauf. Mit 2 GiB hätte der Test im WLAN über 2 Stunden gedauert. Der Vorschlag kam von mir, der Projektverantwortliche hat mit „weiter mit S4“ zugestimmt.
- **Offen: Wartezeit unter Last.** Eine kleine Datei braucht während einer großen Kopie 2,9 s statt 44 ms. Für G1 zählt das nicht, im Explorer würde es aber auffallen.
  - **Belegt im Code:** libsmb2 gewährt jeden verlangten Kredit, liest ungebremst weiter und stellt Antworten hinten an.
  - **Nicht belegt:** welcher Puffer wie viel der Wartezeit ausmacht. Infrage kommen die Sendeschlange, der Sendepuffer von 1 MiB und WLAN und Router.
  - Erst wird gemessen, dann behoben (siehe S4 im Spike-README).
- **G2 (≥ 95 %):** Aus dieser Messung lässt sich G2 nicht sicher ablesen. Beim Schreiben liegt SMB klar über 95 %. Beim Lesen streuen die Runden zu stark, dafür braucht es mehr Runden oder den Median der Verhältnisse je Runde. Gewertet wird ohnehin erst am Build, der 1.0.0 wird.

Das Gate G1 ist damit noch nicht entschieden, denn S6 und S7 fehlen noch.

### S5 entfällt als Live-Test (21.10.2026)

Ruhemodus und Neustart der PS5 werden nicht als eigener Spike live getestet. Stattdessen simulieren skriptgesteuerte Läufe in der CI, dass der Server wegfällt und wiederkommt: beenden, neu starten, Netz weg.

- **Warum:** Der Projektverantwortliche hält einen aktiven Test dafür nicht für nötig.
- **Was die Simulation nicht abdeckt:**
  - ob die PS5 nach dem Ruhemodus die Verbindungen des Payloads erhält
  - ob das Payload nach einem Neustart von selbst startet
  
  Das zeigt sich erst im normalen Betrieb in Phase 5. Das Log des Programms belegt es dort.
- G1 hing ohnehin nicht an S5.

### S6: Der echte Windows-Client läuft in der CI (21.10.2026)

Bestanden (E2). Auf `windows-2025` besteht der echte SMB-Client von Windows das komplette `S3-Check.cmd`, dasselbe Skript wie live auf der PS5. Der Linux-Kernel-Client (cifs) liest und schreibt, eine Auswahl aus smbtorture läuft mit.

- **Server auf dem Windows-Runner:** der statische Linux-Build in WSL 1 mit einem minimalen Alpine-Rootfs, gepinnt per Prüfsumme.
  - **Warum:** Es ist derselbe Code und dieselbe POSIX-Semantik wie bei den Linux-Tests, ohne eigene Windows-Schicht für das Backend.
  - **Verworfen:**
    - **Windows-Build mit llvm-mingw:** Das bräuchte eine Windows-Schicht nur für Tests, und NTFS verhält sich anders als das Dateisystem der PS5.
    - **MSYS2:** NTFS ohne Unterscheidung von Groß- und Kleinschreibung, dazu eine zweite Toolchain.
    - **Eine fertige Action für WSL:** Das wäre Fremdcode in der CI. `wsl --import` reicht.
- **Port:** In der CI 4450 mit `/TCPPORT`, denn Port 445 belegt Windows selbst. Port 445 bleibt Sache von E3.
- **smbtorture:** 13 von 43 Teiltests bestehen. Sie stehen in `tests/spikes/smbtorture-pass.txt`. Fällt einer davon durch, schlägt die CI fehl. Die 30 übrigen sind Arbeit für Phase 2.
- **Ab jetzt** wird `dev-build` nur veröffentlicht, wenn beide Client-Jobs grün sind.

### S7: Die PS5 wird ohne Firewall-Dialog gefunden (21.10.2026)

Bestanden (E3, Build `ff9d50a`, Windows 11 25H2, normaler Benutzer, Firewall-Profil „Öffentlich“). In der CI läuft der Unicast-Teil mit (E2).

- Unicast und Broadcast über UDP werden beide beantwortet, und es erscheint kein Firewall-Fenster. Das Programm belegt keinen festen Port, es verschickt Anfragen und liest die Antworten darauf.
- **Variante 1 funktioniert:** Der Server misst seine Abweichung zur PC-Uhr auf etwa 0,1 s genau, hier 3596,5 s.
- **Für das Programm heißt das:** Die PS5 wird über die gespeicherte IP gefunden. Ändert sich die IP, hilft der Broadcast, und die Server-ID sagt, ob es dieselbe PS5 ist.
- **Noch offen für Phase 2:** der endgültige UDP-Port (im Spike 4446) und eine dauerhafte Server-ID statt einer zufälligen je Start.

## Gate G1: Go für natives SMB (21.10.2026)

Der Projektverantwortliche hat G1 freigegeben. Erfüllt waren S1 bis S4, S6 und S7, S5 entfiel. Der Rückfallweg (eigenes Protokoll mit WinFsp) wird nicht gebraucht. Ab jetzt entsteht Produktcode, beginnend mit Phase 2 (PS5-Server `prospero-smbd.elf`).

- **Bekannt und offen:**
  - die Wartezeit unter Last (2,9 s)
  - 30 von 43 smbtorture-Teiltests
  - die Sommerzeit-Prüfung ab 26.10.
  - Ruhemodus und Neustart im normalen Betrieb
- **Hinweis des Projektverantwortlichen zum Passwort:** Eine PS5-Benachrichtigung verschwindet und taucht danach **nicht** in der Liste der Benachrichtigungen auf. Ein Passwort, das nur einmal so angezeigt wird, reicht für das Produkt also nicht. Der Weg zum Passwort wird im Entwurf für Phase 2 entschieden.

## Phase 2, Grundsatzentscheidungen (21.10.2026)

### Dienstkanal über TCP 4445 statt UDP

Suche, Lebenszeichen, Uhrzeit und Kopplung laufen über eine TCP-Verbindung auf Port **4445**, nicht über UDP.

- **Warum (Projektverantwortlicher):** TCP ist verbindungsorientiert.
- **Folgen:**
  - Einen Broadcast gibt es mit TCP nicht. Die Suche bleibt deshalb bei UDP, siehe Ergänzung unten. Ausgehende TCP-Verbindungen lösen kein Firewall-Fenster aus. Das hat auch jeder Testlauf mit `curl.exe` auf Port 4445 gezeigt.
- **Ändert:**
  - [Verbinden und Trennen (P8)](#verbinden-und-trennen-p8): Statt der UDP-Liveness gibt es ein Lebenszeichen über TCP 4445. Getrennt wird weiterhin erst, wenn auch der Test auf TCP 445 fehlschlägt.
  - [Uhrabweichung](#uhrabweichung-der-pc-liefert-die-referenzzeit): Die PC-Zeit kommt über diesen Kanal statt per UDP.
- Port 4445 war bisher der Steuerport der Test-Payloads. Im Produkt gehört er dem Dienstkanal.
- **Ergänzt am selben Tag (Projektverantwortlicher):** Gesucht wird weiter per **UDP 4446**, wie in S7 mit Unicast und Broadcast. Die Antwort nennt Server-ID, Name und die Ports. Alles andere läuft über TCP 4445. Damit entfällt das Durchprobieren des Netzes.
- **Geändert am selben Tag:** Die Suche läuft auf **UDP 4445** statt 4446, siehe [Ports und Ablauf der Suche](#ports-und-ablauf-der-suche).

### Ports und Ablauf der Suche

Die PS5 belegt **TCP 445** (SMB), **TCP 4445** (Dienstkanal) und **UDP 4445** (Suche). TCP und UDP haben getrennte Portnummern, dieselbe Zahl stört sich nicht.

- **Ablauf im Programm:**
  1. Im Alltag verbindet es sich per TCP 4445 mit der gespeicherten IP. Antwortet dort dieselbe Server-ID, ist die PS5 gefunden.
  2. Nur wenn das scheitert, etwa bei der Einrichtung oder nach einem IP-Wechsel, sucht es per UDP-Broadcast.
  3. Als letzter Ausweg gibt der Benutzer die IP von Hand ein, etwa wenn der Router Broadcasts filtert.
- **Warum:** Auf meine Frage nach einem sinnvollen Aufbau hat der Projektverantwortliche diesem Vorschlag mit „dann leg los“ zugestimmt. Eine Nummer für beides ist leichter zu merken und in einer Firewall freizugeben. Ein Router oder Mesh-System, das Broadcasts filtert, stört so nur die erste Suche.
- **Grenze:** Ein Gastnetz oder eine Client-Isolation im Router trennt PC und PS5 ganz. Dann hilft keine Portwahl.
- **Ersetzt:** UDP 4446 aus der Ergänzung oben. Der Spike S7 bleibt bei 4446, wie er gemessen wurde.

### Kopplung ohne Code (Variante C)

Das Passwort wird nie angezeigt oder abgetippt. Der erste PC, der sich nach dem ersten Start über den Dienstkanal meldet, bekommt Benutzer und Passwort. Die PS5 meldet danach „gekoppelt mit …“.

- **Warum (Projektverantwortlicher):** Im privaten WLAN ist jeder selbst für die Sicherheit verantwortlich. Wer so etwas in einem öffentlichen WLAN macht, handelt ohnehin fahrlässig. Das Risiko, dass ein anderes Gerät im WLAN zuerst koppelt, trägt bewusst der Benutzer.
- **Verworfen:**
  - **A, Passwort wiederholt anzeigen:** Ein langes Passwort ist mühsam abzutippen.
  - **B, Kopplung mit Code:** mehr Aufwand, ohne dass der Projektverantwortliche den Schutz braucht.
- Wann das Koppeln offen ist und wie ein zweiter PC dazukommt, steht im [Entwurf für Phase 2](entwurf-phase2.md).

### Entwurf Phase 2 bestätigt

Der Projektverantwortliche hat die Vorschläge aus dem [Entwurf für Phase 2](entwurf-phase2.md) bestätigt, mit einer Änderung:

- **Freigabe `prospero`** statt `PS5`, also `\\<PS5-IP>\prospero`.
- **Kopplung:** Das erste Koppeln bleibt offen, bis ein PC gekoppelt ist. Ein weiterer PC kommt über „Weiteren PC koppeln“ auf einem gekoppelten PC dazu, danach ist das Koppeln 10 Minuten offen. Zurückgesetzt wird durch Löschen von `config.ini`.
- **Lebenszeichen:** alle 5 s. Die PS5 gilt als weg nach 15 s ohne Antwort und einem fehlgeschlagenen Test auf TCP 445.
- **Uhr:** Die Abweichung kommt nur aus Lebenszeichen mit gültiger Prüfsumme (HMAC-SHA256 mit dem Passwort).
- **Ordner** `/data/prospero-smbd/` für Konfiguration und Log.

### Regel: nur libkernel-Funktionen auf der PS5

Das SDK enthält in `libc.a` für jede FreeBSD-Systemaufrufnummer einen nackten Aufruf. Diese Aufrufe setzen `errno` nicht, und auf der PS5 kann eine Nummer etwas anderes bedeuten. In S3 hat `utimensat()` deshalb nichts getan.

- **Regel:** Auf der PS5 nur Funktionen benutzen, die libkernel exportiert. Die CI prüft das (`.github/scripts/check-raw-syscalls.sh`).
- **Ebenso:** Neue Bibliotheksfunktionen gelten erst als sicher, wenn sie auf der PS5 gelaufen sind. Nach `localtime_r()` blieb das Payload stumm.

## Ziele nach Messung 2 am Kabel (23.10.2026)

Der Projektverantwortliche hat die PS5 per Kabel angeschlossen (feste IP 192.168.188.163), um zuerst unter den besten Bedingungen zu arbeiten und später für das WLAN zu optimieren.

- **Vorrangiges Ziel des nächsten Schritts:** am Kabel **≥ 95 % der Geschwindigkeit von ftpsrv**, lesend und schreibend.
- **Danach:** Die Verbindung wird sofort auf- und abgebaut, wenn sich der Zustand der PS5 ändert (Phase 3). Bisher gilt aus dem Entwurf Phase 2: Lebenszeichen alle 5 s, weg nach 15 s ohne Antwort und fehlgeschlagenem Test auf TCP 445. Was „sofort“ in Sekunden heißt, wird dort festgelegt.
- **Stand am Kabel (E3, Build `379f98c`):** SMB liest 26–28 MB/s und schreibt 25 MB/s, ftpsrv 114 und 89 MB/s, also etwa 24 % und 28 %. Der Server rechnete beim Lesen 87 % der Zeit. libsmb2 signiert mit einer Referenz-AES, die den Schlüssel für jeden 16-Byte-Block neu berechnet. Im WLAN fiel das nicht auf, weil die Funkstrecke langsamer war als diese Grenze; G1 wurde im WLAN gemessen.
- **Maßnahme:** AES-CMAC mit AES-NI (Patch `libsmb2-cmac-aesni`), Signieren bleibt immer an. Der Server prüft beim Start die Prüfwerte aus RFC 4493 und vergleicht mit der Referenz, sonst nimmt er die Referenz.
- **Gates G1/G2** bleiben im Text als WLAN-Messung definiert, bis der Projektverantwortliche das ändert. Jede Messung nennt künftig WLAN oder Kabel.

### Ergebnis (24.10.2026)

Ziel am Kabel erreicht (E3, Build `86e62c1`, 7 Runden im Wechsel): Schreiben 136 %, Lesen 101 % von ftpsrv, keine Leserunde unter 95 %. Details in Messung 8 im [Fahrplan](fahrplan.md).

## „Sofort“ für Verbinden und Trennen (24.10.2026)

Der Projektverantwortliche hat den Vorschlag angenommen:

- **Erscheinen:** `P:` ist höchstens **3 s** nach Erreichbarkeit da. Solange die PS5 fehlt, versucht das Programm jede Sekunde eine Verbindung.
- **Verschwinden am Kabel:** höchstens **5 s**. Es gibt jede Sekunde ein Lebenszeichen. Getrennt wird nach 3 verpassten, und nur wenn auch der Test auf TCP 445 scheitert.
- **Verschwinden im WLAN:** 10–15 s, weil kurze Aussetzer dort normal sind und `P:` nicht mitten in einer Kopie verschwinden soll.
- **Ordentliches Ende des Payloads:** Es meldet sich über den Dienstkanal ab, `P:` ist nach unter 1 s weg.
- **Bedingung des Projektverantwortlichen:** Das darf weder auf der PS5 noch auf dem PC spürbar CPU kosten, „ganz entspannt für beide Maschinen“. Das wird gemessen, nicht angenommen:
  - **PC:** Das Programm schläft zwischen den Lebenszeichen in einem Wartezustand, es fragt nicht in einer Schleife ab. Ziel ist eine Rechenzeit unter 0,1 % eines Kerns und kein Aufwachen öfter als einmal je Sekunde.
  - **PS5:** Ein Lebenszeichen ist eine Zeile von etwa 100 Bytes je Sekunde, ohne Signaturrechnung über große Daten. Ziel ist eine Rechenzeit des Payloads unter 0,1 % eines Kerns im Leerlauf mit verbundenem PC. Die Serverschleife wacht heute ohne Last alle 100 ms auf (Vorgabe von libsmb2). Das wird zuerst gemessen und nur verlängert, wenn es messbar Last erzeugt.
- **Offen:** Woran das Programm erkennt, ob Kabel oder WLAN gilt. Der PC kennt nur seine eigene Verbindung, nicht die der PS5. Vorschlag: Die PS5 meldet ihre Verbindungsart im `HELLO`, falls sie das ermitteln kann. Sonst gilt die vorsichtige WLAN-Zeit.
- **Messung P30, Kabel** (E3, Build `aef1906`, 24.10.2026, Freigabe verbunden, nichts übertragen):
  - Ohne Lebenszeichen, 30 s: Server 0,067 % eines Kerns, PC 0,000 %. Das ist die Serverschleife, die alle 100 ms aufwacht.
  - Lebenszeichen jede Sekunde, 120 s: Server 0,080 %, PC 0,038 % (mit Fortschrittsbalken des Testprogramms). Alle 120 `PING` beantwortet. Das Lebenszeichen selbst kostet auf der PS5 also etwa 0,013 %.
  - Ziel unter 0,1 % erreicht. Die PS5 hat 8 Kerne, bezogen auf die ganze Konsole sind das etwa 0,01 %.
  - Verbindungsart: `getifaddrs` läuft auf der PS5. Am Kabel heißt die Schnittstelle `eth0`, Typ Ethernet. `SIOCGIFMEDIA` liefert nichts. Die WLAN-Schnittstelle erscheint nur, wenn sie verbunden ist.
- **Messung P30, WLAN** (E3, Build `b56cd09`, 24.10.2026): Die Schnittstelle heißt `wlan0` und meldet ebenfalls Typ Ethernet. Also entscheidet der Name, `eth…` ist Kabel, `wlan…` ist WLAN. Leerlauf: Server 0,037 % ohne und 0,045 % mit Lebenszeichen, PC 0,038 %. Das Lebenszeichen kam nach 4–5 ms zurück (Kabel: 0–1 ms).
- **Umsetzung:** Das Payload nennt die Verbindungsart im `HELLO` (`link=wired|wifi|unknown`). Das Programm nimmt bei `wired` die Kabelzeit, sonst die WLAN-Zeit.

## Kein automatischer Rundruf (24.10.2026)

Der Projektverantwortliche möchte das Heimnetz nicht mit Rundrufen belasten, wenn die PS5 einfach aus ist. Sein erster Vorschlag war ein Dialog nach 30 s ohne Antwort. Dagegen sprach: Die PS5 ist meist aus, der Dialog käme dann nach jedem PC-Start, und das Programm kann „aus“ nicht von „neue IP“ unterscheiden. Entschieden ist der Weg über das Tray:

- Im Alltag nur Verbindungsversuche an die gespeicherte IP, alle 2 s.
- Das Tray-Symbol zeigt „nicht erreichbar, zuletzt gesehen am …“.
- Ein Rundruf läuft nur über „PS5 im Netz suchen (30 s)“ im Tray-Menü und einmal bei der Einrichtung.

## Laufwerk, Installation, Deinstallation (24.10.2026)

- **`P:` belegt:** Das fremde Laufwerk bleibt unberührt. Das Programm nimmt den nächsten freien Buchstaben rückwärts ab `O:` und meldet das einmal.
- **Installation:** Ein Installationsdialog mit wählbarem Zielordner, keine stille Selbstkopie. Danach muss man das Programm nie wieder anfassen. Die exe ist ihr eigener Installer, es gibt kein separates Installer-Paket.
- **Admin-Rechte für die Installation erlaubt** (Projektverantwortlicher, 24.10.2026; ändert „braucht keinen Admin“ aus D5 für die Installation):
  - Der Installationspfad ist frei wählbar, vorbelegt mit `C:\Program Files\ProsperoAutoDrive\`, ausdrücklich nicht `%LOCALAPPDATA%`. Braucht der gewählte Ordner Admin-Rechte, fragt der Installer selbst per UAC. Ein Rechtsklick „Als Administrator“ ist nicht nötig.
  - **Das Programm selbst läuft weiter ohne Admin.** Ein Laufwerk, das ein Prozess mit Admin-Rechten verbindet, sieht der normale Explorer nicht. Das ist eine Eigenschaft von Windows, keine Wahl.
  - Ohne Signatur zeigt der UAC-Dialog „Unbekannter Herausgeber“, bis die Signierung (Phase 4) steht.
- **Deinstallation:** über Windows (Installierte Apps / „Programm deinstallieren“), per Eintrag für den Benutzer unter `HKCU`.

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
