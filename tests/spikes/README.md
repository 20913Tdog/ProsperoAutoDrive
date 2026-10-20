# Machbarkeits-Spikes

Die Spikes S1–S7 klären vor jedem Produktcode, ob die Architektur „natives SMB“ trägt (Gate G1). Jeder Spike nennt seine Evidenzebene:

- **E1:** CI unter Linux
- **E2:** CI mit echtem Windows-Client
- **E3:** echte PS5

## Test mit einem Klick

`tools\Run-Test.cmd` starten, per Doppelklick oder in der Konsole. Beim ersten Mal fragt es nach der IP-Adresse der PS5 und speichert sie in `tools\ps5.local.ini` (nicht in git). Danach läuft alles ohne Rückfragen:

1. `git pull` auf `dev`
2. den geprüften Build holen. Er muss genau zum Stand passen, sonst bricht das Skript ab, weil die CI noch läuft.
3. alle Verbindungen zur PS5 trennen und ein laufendes Testpayload beenden
4. das Payload senden (ELF-Loader, Port 9021) und warten, bis es bereit ist
5. das Passwort einmalig über Port 4445 abholen, den aktuellen Test ausführen und das Payload danach beenden

Am Ende steht die komplette Ausgabe in der Zwischenablage und in `logs\<Test>.log`. Sie muss nur noch in den Chat eingefügt werden. Explorer-Fenster zur PS5 vorher schließen.

Welcher Test läuft, steht in `DEFAULT_TEST` im Skript. Ein anderer Test lässt sich direkt angeben: `tools\Run-Test.cmd S2 [PS5-IP]`. Die Einzelschritte in den Abschnitten unten bleiben als Weg von Hand erhalten.

## S1 – Kann ein Payload TCP 445 belegen?

**Frage:** Darf ein Payload auf der PS5 den SMB-Port 445 (und den Ausweichport 4445) öffnen, und ist 445 nicht schon von etwas anderem belegt?

**Was die Probe tut:**

- Das Payload `prospero-s1-portprobe.elf` versucht, 445 und 4445 zu belegen, und zeigt das Ergebnis als PS5-Benachrichtigung an.
- Auf jedem belegten Port beantwortet es einfache HTTP-Anfragen, mit dem Ergebnis für beide Ports und dem Fehlergrund.
- Es schreibt **keine Dateien** und beendet sich nach 10 Minuten von selbst.

**Bereits geprüft (E1, CI unter Linux):** derselbe Quellcode mit freien Ports, mit 445 durch einen Fremddienst belegt, und bei einem Neustart über eine laufende Instanz hinweg.

### Ablauf auf deinem PC

1. Auf `dev` aktualisieren und den geprüften Build holen:
   ```
   git pull
   tools\Get-DevBuild.cmd
   ```
   Alle Zeilen müssen `OK` zeigen. Das Payload liegt danach in `dist\dev\`.
2. **Vorher-Messung** (Payload noch nicht gestartet):
   ```
   tests\spikes\S1-Check.cmd <PS5-IP>
   ```
   Erwartet wird auf beiden Ports „Nichts lauscht“. Antwortet auf 445 schon jetzt etwas, ist das bereits ein Ergebnis: 445 ist belegt.
3. **Payload starten:** so wie du sonst Payloads lädst, oder mit dem mitgelieferten Helfer an den ELF-Loader (Port 9021):
   ```
   tools\Send-Payload.cmd <PS5-IP> dist\dev\prospero-s1-portprobe.elf
   ```
   Auf der PS5 erscheint `Prospero S1: 445 OK, 4445 OK (uid 0)` oder eine Fehlermeldung.
4. **Nachher-Messung:**
   ```
   tests\spikes\S1-Check.cmd <PS5-IP>
   ```
5. Die komplette Ausgabe beider Messungen und den Text der PS5-Benachrichtigung zurückmelden.

### Auswertung

| Ergebnis | Bedeutung | Konsequenz |
|---|---|---|
| Vorher: 445 „Nichts lauscht“; nachher: `bind 445: OK` auf beiden Ports | **S1 bestanden** | Primärpfad Port 445 |
| `bind 445: FAIL … Address already in use` | Fremddienst auf 445 | Fallback-Entscheidung: Alternativport 4445 (nur Windows 11 24H2+) oder Fremddienst identifizieren |
| `bind 445: FAIL … Permission denied` | Keine Rechte für Port < 1024 | Rechte im Payload anheben, erneut testen |
| 4445 erreichbar, 445 läuft in Timeout | Port 445 wird unterwegs gefiltert (z. B. vom Router oder WLAN-Gerät) | Netz prüfen, Alternativport bewerten |
| Beide Ports Timeout | PS5 nicht erreichbar oder Payload nicht gestartet | IP und Payload-Start prüfen |

### Ergebnis: bestanden (E3, 20.10.2026, Build `95dabbe`)

Nachher-Messung vom Windows-PC zur PS5 im WLAN, auf beiden Ports identisch bis auf `connected_port`:

```
PROSPERO-S1 OK
connected_port=445
bind 445: OK
bind 4445: OK
uid=1 euid=0
```

**Bewertung:**
- Das Payload belegt 445 mit effektiven Root-Rechten (`euid=0`), und Windows erreicht 445 über das WLAN, also ohne Filterung unterwegs.
- Ein Fremddienst auf 445 ist ausgeschlossen. Selbst ein Dienst, der nur an die konkrete IP der PS5 gebunden wäre, hätte die Verbindung vor unserer Wildcard-Bindung bekommen.
- **Konsequenz:** Port 445 ist der Primärpfad, der Alternativport entfällt. Für S3 notiert: Prüfen, mit welchem Besitzer und welchen Rechten neu angelegte Dateien entstehen (`uid=1`, `euid=0`).

## S2 – Bedient die libsmb2-Server-Engine den Windows-Client korrekt?

**Frage:** Liefert die libsmb2-Server-Engine mit unserem eigenen Dateisystem-Backend einen Ordner korrekt und vollständig an den eingebauten SMB-Client von Windows 11 aus, mit NTLMv2 und Pflicht-Signing? S2 prüft nur das Lesen; Schreiben folgt in S3.

**Was das Payload tut:**

- `prospero-s2-smbserver.elf` legt einen **neuen** Testordner `/data/_ProsperoAutoDrive_Test_<Zeit>-<Zufall>` an und füllt ihn mit festen Testdateien, zusammen etwa 65 MB (Liste in `ps5/spikes/smbserver/testdata.c`).
- Genau dieser Ordner wird **schreibgeschützt** als `\<PS5-IP>\s2` freigegeben. Benutzer `prospero`; das Passwort erscheint als PS5-Benachrichtigung.
- Port 4445 zeigt den Status, auch die Anfragen, die das Backend noch nicht unterstützt (`unsupported:`), und warum die letzten Verbindungen endeten (`connection_end:`).
- Es beendet sich nach 15 Minuten von selbst, früher mit `curl.exe http://<PS5-IP>:4445/quit`.
- Außerhalb des neuen Testordners liest und schreibt es nichts. Der Testordner bleibt danach auf der PS5 liegen. Er wird nicht automatisch gelöscht; ob und wann er weg soll, entscheidest du.

**Bereits geprüft (E1, CI unter Linux, Samba-`smbclient` mit Pflicht-Signing):** vollständiger rekursiver Download mit SHA-256-Vergleich, die Dialekte SMB 2.0.2 bis 3.1.1, eine Ordnerliste mit 1500 Einträgen, Groß-/Kleinschreibung, Ablehnung von Anlegen, Löschen und Umbenennen, keine Pfade aus der Freigabe heraus, falsches Passwort, unbekannte Freigabe, Datei- und Laufwerksinformationen.

### Ablauf auf deinem PC

1. Auf `dev` aktualisieren und den geprüften Build holen:
   ```
   git pull
   tools\Get-DevBuild.cmd
   ```
2. Payload starten:
   ```
   tools\Send-Payload.cmd <PS5-IP> dist\dev\prospero-s2-smbserver.elf
   ```
   Auf der PS5 erscheint `Prospero S2 bereit: Freigabe s2 (nur lesen), Benutzer prospero, Passwort xxxx-xxxx-xxxx (15 min)`.
3. Prüfskript starten und das Passwort eingeben, wenn es danach fragt:
   ```
   tests\spikes\S2-Check.cmd <PS5-IP>
   ```
4. Bei Schritt 7 wartet das Skript. Jetzt im Explorer `\<PS5-IP>\s2` in die Adresszeile eingeben und:
   - den Ordner `viele` öffnen und bis ans Ende scrollen (1500 Dateien),
   - den `Ordner mit Leerzeichen` öffnen: Wird `Grüße äöü €.txt` richtig angezeigt?
   - `hello.txt` per Doppelklick öffnen,
   - `pattern-64MiB.bin` auf den Desktop kopieren, die Dauer grob notieren und die Kopie danach löschen,
   - Rechtsklick auf `tief` → Eigenschaften,
   - eine beliebige Datei vom Desktop in den Ordner ziehen: Das muss scheitern.

   Danach im Skriptfenster eine Taste drücken.
5. Die **komplette Skriptausgabe** und alles, was im Explorer auffällig war (Fehlermeldungen, Wartezeiten, falsche Anzeige), zurückmelden.

### Auswertung

| Ergebnis | Bedeutung | Konsequenz |
|---|---|---|
| `S2-CHECK: ALLES OK`, Explorer unauffällig | **S2 bestanden** | weiter mit S3 (Schreiben) |
| Schritt 1: Systemfehler 1326 | Anmeldung abgelehnt | Passwort prüfen. Wenn es stimmt: NTLMv2-Problem, analysieren |
| Schritt 1: Systemfehler 53 oder 67 | Server oder Freigabe nicht gefunden | Läuft das Payload? IP prüfen |
| Schritt 1: anderer Fehler | z. B. Signing- oder Dialekt-Problem | Fehlertext analysieren, Blocker für S2 |
| `FALSCH` oder `FEHLT` bei den Prüfsummen | Daten kommen falsch an | Blocker |
| `unsupported:`-Zeilen im Status | Windows fragt etwas, das das Backend nicht kann | einzeln bewerten |
| sehr hoher Zähler bei `CHANGE_NOTIFY` | Explorer wiederholt die Änderungsüberwachung in einer Schleife | ausstehende Benachrichtigungen in S3 einbauen |

### Bekannte Grenzen dieses Prototyps

- Änderungsbenachrichtigungen (`CHANGE_NOTIFY`) werden abgelehnt; der Explorer aktualisiert die Ansicht dann nur mit F5.
- Nach einem falsch eingegebenen Passwort kann ein zweiter Versuch auf derselben Verbindung scheitern (libsmb2, SMB 3.1.1). Dann `net use \<PS5-IP>\s2 /delete` und neu verbinden.
- libsmb2 schickt die Antworten auf zusammengesetzte Anfragen (Compound) einzeln statt gebündelt. Ob Windows das ohne Nachteile annimmt, zeigt erst dieser Test.

### Ergebnis: funktional bestanden (E3, 20.10.2026, Build `dc283f6`)

Windows 11 Pro 25H2 (Build 26200) im WLAN, `S2-CHECK: ALLES OK`, Explorer-Test unauffällig.

**Bewertung:**
- Die Anmeldung mit NTLMv2 funktioniert. Windows 11 Pro verlangt Signing, die Sitzung war also signiert. Das fehlende SPNEGO-mechListMIC stört Windows nicht, anders als Sambas `smbclient`.
- Alle 1507 Dateien kamen mit korrekter SHA-256 an. Groß-/Kleinschreibung, Unicode-Namen und der Ordner mit 1500 Einträgen funktionieren, und alle Schreibversuche wurden abgelehnt.
- Der Status zeigt `lost_replies=0` und keine abgebrochenen Verbindungen.

**Was Windows zusätzlich anfragt (`unsupported:`):**

| Anfrage | Bedeutung | Bewertung |
|---|---|---|
| `QUERY_DIRECTORY class 0x51` ×16 | FileIdAllExtdBothDirectoryInformation (Windows 11) | Windows fällt sauber auf 0x25 zurück. Nachrüsten spart eine Anfrage je Ordner |
| `QUERY_INFO type 3` ×1508 | Sicherheitsbeschreibung, robocopy fragt je Datei | unkritisch. Im Produkt eine einfache ACL liefern (Explorer-Reiter „Sicherheit“) |
| `CREATE on IPC$ srvsvc` ×74, `wkssvc` ×6 | Freigabe- und Arbeitsstationsinfos per RPC | für ein Laufwerk unkritisch. Für das Produkt prüfen, denn 74 Fehlversuche kosten Wartezeit |
| `CHANGE_NOTIFY` ×12 | Änderungsüberwachung des Explorers | **keine Schleife**: Windows akzeptiert die Ablehnung. Live-Aktualisierung später |
| `IOCTL 0x000900C0` ×12 | FSCTL_CREATE_OR_GET_OBJECT_ID (Verknüpfungsverfolgung) | unkritisch |
| `IOCTL 0x00094264` ×2 | FSCTL_OFFLOAD_READ (ODX-Kopierbeschleunigung) | unkritisch, Windows kopiert dann normal |
| `IOCTL 0x00110018` ×1 | FSCTL_PIPE_WAIT auf IPC$ | unkritisch |

**Geschwindigkeit: viel zu langsam.**
- robocopy brauchte für alle 65 MB 5:38 min (≈ 0,2 MB/s). Ohne die 64-MiB-Datei sind das ≈ 180 ms je kleiner Datei.
- Der Explorer brauchte für 64 MiB ≈ 65 s (≈ 1 MB/s, stark schwankend).
- So ist Gate G1 nicht erreichbar. Deshalb kommt die Ursachenanalyse S2b vor S3.

**Notiert für S3:** Die Dateizeiten der PS5 passten nicht zur robocopy-Endzeit. Die Messung in S2b zeigt den Grund: Intern geht die PS5 1 Stunde vor (siehe dort). Für Zeitstempel beim Schreiben ist das wichtig.

## S2b – Warum ist das Lesen so langsam?

**Frage:** Liegt die Geschwindigkeit von S2 an zu kleinen Socket-Puffern auf der PS5, an der SMB3-Signatur (AES-CMAC, in libsmb2 ohne Hardwarebeschleunigung) oder am WLAN selbst?

**Hintergrund:**
- ftpsrv stellt für jede Datenverbindung 1 MiB Sende- und Empfangspuffer ein. Das deutet auf kleine Standardpuffer der PS5 hin.
- libsmb2 berechnet AES-CMAC mit einer einfachen Software-AES und erweitert dabei den Schlüssel für jeden 16-Byte-Block neu. Das kostet viel Rechenzeit je MiB.

**Was das neue Payload zusätzlich kann:** Über Port 4445 lässt es sich zwischen drei Profilen umschalten. Es zählt Rechenzeit (`cpu_user_ms`, `cpu_sys_ms`), Lesezeit von der SSD (`pread_ms`) und das Zeitfenster aller READs (`read_window_ms`).

| Profil | Socket | SMB-Version und Signatur |
|---|---|---|
| a | Standard der PS5, genau wie in S2 | 3.1.1, AES-CMAC |
| b | 1 MiB Puffer, TCP_NODELAY (wie ftpsrv) | 3.1.1, AES-CMAC |
| c | wie b | 2.1, HMAC-SHA256 |

**Bereits geprüft (E1, CI):** Umschalten aller Profile, ausgehandelter Dialekt (3.1.1 bzw. 2.1) und die Zähler.

### Ablauf auf deinem PC

Am einfachsten mit `tools\Run-Test.cmd` (siehe oben), denn S2b ist der aktuelle Test. Von Hand:

1. Auf `dev` aktualisieren und den Build holen:
   ```
   git pull
   tools\Get-DevBuild.cmd
   ```
2. Das Payload **neu** starten, damit Profil a unverändert ist:
   ```
   tools\Send-Payload.cmd <PS5-IP> dist\dev\prospero-s2-smbserver.elf
   ```
3. Alle Explorer-Fenster schließen, die auf die PS5 zeigen. Dann das Messskript starten und das Passwort aus der PS5-Benachrichtigung eingeben:
   ```
   tests\spikes\S2-Speed.cmd <PS5-IP> [FTP-Port]
   ```
   Der FTP-Port ist optional (Standard 2121, ftpsrv). Er gilt für den FTP-Server, der auf der PS5 schon läuft und die WLAN-Referenz liefert. Das Skript trennt zuerst alle bestehenden Verbindungen zur PS5. Windows lässt pro Server nur einen Benutzernamen zu (Systemfehler 1219).

   Das dauert einige Minuten. Am PC und auf der PS5 in der Zeit möglichst nichts im Netz laden, keine Downloads und kein Streaming.
4. Die komplette Ausgabe bis `S2-SPEED: FERTIG` zurückmelden.

### Auswertung

| Beobachtung | Bedeutung | Konsequenz |
|---|---|---|
| b deutlich schneller als a | Die Socket-Puffer bremsen | Profil b übernehmen |
| c deutlich schneller als b, `cpu_user_ms` nahe `read_window_ms` | Die Signatur rechnet zu langsam | schnelle AES-CMAC (AES-NI) in libsmb2 einbauen, als Beitrag für upstream |
| a, b und c ähnlich langsam wie die FTP-Referenz | Das WLAN begrenzt | Vergleich mit ftpsrv in S4 entscheidet |
| alle deutlich langsamer als die FTP-Referenz, CPU niedrig | Wartezeiten im Protokollablauf | Ablauf genauer untersuchen |

### Ergebnis (E3, 20.10.2026, Build `5c2412e`, WLAN, je Profil 2 Läufe)

| Messung | FTP (ftpsrv) | a | b | c |
|---|---|---|---|---|
| 64 MiB, Lauf 1 | 5,2 MB/s (vorher) | 2,7 MB/s | 2,1 MB/s | 3,6 MB/s |
| 64 MiB, Lauf 2 | 3,4 MB/s (nachher) | 0,8 MB/s | 3,8 MB/s | 2,9 MB/s |
| 100 kleine Dateien | – | 15 s | 6 s | 8 s |
| CPU je 64 MiB (`cpu_user_ms`) | – | ≈ 1,95 s | ≈ 1,9 s | ≈ 0,58 s |
| SSD-Lesezeit je 64 MiB (`pread_ms`) | – | ≈ 50 ms | ≈ 50 ms | ≈ 60 ms |

**Bewertung:**
- **Kleine Dateien:** b ist 2,5-mal schneller als a, etwa 60 statt 150 ms je Datei. Das passt zu TCP_NODELAY: Ohne diese Option halten Nagle-Algorithmus und verzögerte Bestätigungen jede kleine Antwort auf. **Profil b wird übernommen.**
- **Signatur:** AES-CMAC kostet gut dreimal so viel Rechenzeit wie HMAC-SHA256, etwa 30 ms je MiB. Das begrenzt bei rund 33 MB/s, im WLAN bremst es also noch nicht. Für G2 und kabelgebundenes Netz bleibt eine schnelle AES-CMAC nötig. SMB 3.1.1 bleibt.
- **WLAN:** Die SSD bremst nicht. Das WLAN schwankt stark: Schon die FTP-Referenz fiel von 5,2 auf 3,4 MB/s. Mit zwei Läufen je Profil lässt sich der Abstand zu ftpsrv nicht belastbar angeben. Die SMB-Läufe mit b und c lagen bei 2,1 bis 3,8 MB/s, also grob 50 bis 90 % der FTP-Werte. Das klärt erst S4, mit ≥ 7 abwechselnden Läufen und Median.
- **Uhr:** Die Uhr der PS5 (`now_utc`) geht gegenüber echter UTC (github.com) genau **1 Stunde vor**, PC und Anzeige der PS5 stimmen. Vermutlich rechnet die PS5 die Sommerzeit nur in die Anzeige ein. Für S3 wichtig, denn Zeitstempel, die die PS5 selbst setzt, lägen in der Sommerzeit 1 Stunde daneben.

## S3 – Schreibt Windows korrekt auf die PS5?

**Frage:** Kann der Windows-Client über unser Backend Dateien und Ordner auf der PS5 anlegen, schreiben, überschreiben, umbenennen, verschieben und löschen? Kommen alle Bytes und die Änderungszeiten richtig an? Wem gehören neu angelegte Dateien, und welche Uhrzeit bekommen sie?

**Was das Payload tut:**

- `prospero-s3-smbserver.elf` legt wie in S2 einen **neuen** Testordner `/data/_ProsperoAutoDrive_Test_<Zeit>-<Zufall>` mit denselben Testdateien an. Diesen Ordner gibt es als `\<PS5-IP>\s3` frei, **zum Lesen und Schreiben**.
- Geschrieben wird nur innerhalb dieses Ordners, denn Namen, die aus der Freigabe hinausführen, lehnt der Server ab. Der Ordner bleibt danach liegen. Ob und wann er weg soll, entscheidest du.
- Socket-Einstellungen wie Profil b aus S2b. Das Payload läuft 30 Minuten.
- Nicht in diesem Prototyp:
  - Freigabemodi und Bereichssperren werden angenommen, aber nicht durchgesetzt.
  - Attribute außer Ordner und Archiv sowie die Erstellungszeit werden nicht gespeichert, nur gezählt.
  - Alternative Datenströme gibt es nicht.

**Bereits geprüft (E1, CI unter Linux, Samba-`smbclient`, SMB 3.1.1 mit Signing):**

- Hochladen aller 1507 Testdateien mit SHA-256-Vergleich auf dem Server
- Überschreiben kürzt die Datei, die vorhandene Schreibweise bleibt erhalten
- Ordner anlegen, auch die Ablehnung eines Ordners, der sich nur in Groß-/Kleinschreibung unterscheidet
- Nicht leere Ordner werden nicht gelöscht, ein Ordnerbaum lässt sich löschen
- Umbenennen, auch nur der Groß-/Kleinschreibung, Verschieben, Ersetzen nur auf Wunsch, Ordner umbenennen
- Löschen
- Vom Client gesetzte Änderungszeit
- Kein Schreiben außerhalb der Freigabe, keine ungültigen Namen
- Besitzer neuer Dateien

### Ablauf auf deinem PC

`tools\Run-Test.cmd` starten (siehe oben), denn S3 ist der aktuelle Test. Explorer-Fenster zur PS5 vorher schließen. Der Lauf dauert etwa 10 Minuten.

Was `tests\spikes\S3-Check.cmd` dabei prüft:

1. **Vorlage holen:** Die Testdateien werden auf den PC kopiert.
2. **Hochladen:** Mit robocopy geht alles in den neuen Ordner `hochgeladen`.
3. **Größen und Zeiten:** Größe und Änderungszeit jeder Datei müssen stimmen. Das prüft ein Probelauf von robocopy.
4. **Byte für Byte:** Alles wird wieder heruntergeladen und mit den Prüfsummen verglichen.
5. **Bearbeiten im Ordner `ops`:** anlegen, anhängen, überschreiben, innerhalb der Freigabe kopieren, umbenennen, nur die Groß-/Kleinschreibung ändern, verschieben, Ordner umbenennen und löschen. Ein nicht leerer Ordner darf nicht gelöscht werden. Ein kleiner Ordnerbaum, den der Test gerade selbst angelegt hat, wird rekursiv gelöscht.
6. **Uhrzeit:** Eine neue Datei bekommt ihre Zeit von der Uhr der PS5. Das Skript zeigt sie neben der PC-Zeit und neben UTC von github.com.
7. **Tempo:** 64 MiB werden per SMB und zum Vergleich per FTP (ftpsrv, Port 2121) in den Testordner geschrieben.

### Auswertung

| Ergebnis | Bedeutung | Konsequenz |
|---|---|---|
| `S3-CHECK: ALLES OK` | **S3 funktional bestanden** | Zeitstempel und Tempo bewerten, weiter mit S4 |
| Fehler beim Hochladen oder `FALSCH` bei Bytes, Größen oder Zeiten | Daten oder Zeiten kommen falsch an | Blocker, analysieren |
| einzelne `FALSCH` in Schritt 5 | Ein Dateivorgang verhält sich anders als unter Windows | einzeln bewerten |
| Uhrzeit der neuen Datei 1 Stunde daneben | Die Uhr der PS5 schlägt durch, siehe S2b | Ausgleich im Server entwerfen |
| `unsupported:`- oder `delete_failures`-Zeilen im Status | Windows fragt etwas, das das Backend nicht kann | einzeln bewerten |

### Erster Lauf (E3, 20.10.2026, Build `1ec44bd`, WLAN)

Build `20de186` blieb nach dem Start stumm: Die PS5 zeigte „bereit“, doch auf Port 4445 kam keine Antwort. Wahrscheinliche Ursache war `localtime_r()`, die einzige neue Bibliotheksfunktion auf diesem Weg. Seit `1ec44bd` ist sie entfernt, und das Payload antwortet. Das bestätigt die Vermutung nicht sicher, passt aber dazu.

**Funktioniert:**
- Alle 1507 Dateien kamen hochgeladen und wieder heruntergeladen Byte für Byte gleich an.
- Alle 13 Dateivorgänge in `ops` liefen wie unter Windows, auch die Änderung nur der Groß-/Kleinschreibung, das Verschieben, die Ablehnung beim Löschen eines nicht leeren Ordners und das rekursive Löschen.
- Kopieren innerhalb der Freigabe klappt: Windows fällt nach der abgelehnten serverseitigen Kopie (`IOCTL 0x00140078`) auf Lesen und Schreiben zurück.
- **Tempo Schreiben, 64 MiB, ein Lauf:** SMB 6,9 MB/s, FTP (ftpsrv) 6,8 MB/s.

**Nicht bestanden: Änderungszeiten.** Keine einzige der 1507 Zeiten wurde übernommen (`times_set=0`).
- **Ursache:** Das SDK liefert `utimensat()` nur als nackten FreeBSD-Systemaufruf. libkernel der PS5 hat diese Funktion nicht, auf der Konsole kann die Nummer etwas anderes bedeuten, und das Ergebnis landet nicht in `errno`. Darum stand im Status ein veraltetes `errno 35`.
- **Behoben:** Seit dem nächsten Build nutzt das Backend `utimes()` aus libkernel. Damit sind die Zeiten auf die Mikrosekunde genau, Windows rechnet mit 100 ns. Deshalb vergleicht die Prüfung mit 2 Sekunden Toleranz (`/FFT`) und zeigt die genaue Abweichung zusätzlich an.
- **Absicherung:** Die CI bricht jetzt ab, sobald unser Code oder libsmb2 einen solchen nackten Systemaufruf verwendet (`.github/scripts/check-raw-syscalls.sh`).

**Uhrzeit bestätigt:** Eine neue Datei, die ohne Zeitangabe geschrieben wird, bekommt die Zeit der PS5. Im Explorer liegt sie genau **1 Stunde in der Zukunft**: robocopy zeigt 18:47:01 UTC, github.com 17:47:00 UTC. Das muss der Server ausgleichen. `GET /clock` fragt dafür ab, was die PS5 über ihre Zeitzone weiß.

**Weitere Befunde:**
- Neue Dateien gehören `uid=0 gid=0` und haben die Rechte `0666`. Es gibt also keine umask, alle dürfen lesen und schreiben.
- Neu in `unsupported:` ist `IOCTL 0x00140078`, die serverseitige Kopie. Die Sicherheitsabfrage (`QUERY_INFO type 3`) kam 3015-mal, robocopy fragt sie je Datei ab. Beide sind für das Produkt nachzurüsten.

### Ergebnis: bestanden (E3, 20.10.2026, Build `46a55e4`, WLAN)

`S3-CHECK: ALLES OK`.

- Bytes, alle 13 Dateivorgänge und die Kopie innerhalb der Freigabe stimmen wie im ersten Lauf.
- **Änderungszeiten:** Alle 1507 stimmen, sogar ohne Toleranz (`times_set=3030`, zwei Aufrufe je Datei). Die Zeiten der PS5 haben ohnehin nur Mikrosekunden-Auflösung, deshalb verliert `utimes()` hier nichts.
- **Tempo Schreiben, 64 MiB, ein Lauf:** SMB 7,7 MB/s, FTP 7,2 MB/s. Für 1507 Dateien mit 65 MB brauchte das Hochladen 1:40 min, also etwa 66 ms je Datei.
- **Uhr der PS5:** Neue Dateien liegen wieder genau 1 Stunde in der Zukunft. Laut `GET /clock` meldet der Kernel `tz_minuteswest=0` und `tz_dsttime=60`, und `sceKernelGettimezone` liefert dieselben Werte. Die PS5 führt ihre Kernel-Zeit also als „UTC“ und kennt dazu 60 Minuten Sommerzeit. Ihre Uhr geht genau um diese 60 Minuten vor.
  - **Vermutung:** Die Sommerzeit steckt in der Kernel-Uhr selbst. Dann verschwindet die Abweichung nach dem Ende der Sommerzeit am 25.10.2026. Prüfbar ist das mit einem Lauf ab dem 26.10.
  - **Sicher ist:** Die Abweichung ist konstant und lässt sich messen.
- **Für das Produkt** muss der Server die Abweichung in beide Richtungen ausgleichen: abziehen, wenn er Zeiten meldet, und addieren, wenn ein Client Zeiten setzt. So bleiben die Zeiten auf der Platte im Zeitbezug der PS5, passend zu ftpsrv und anderen Programmen, und Windows sieht echte Zeiten. Woher der Server die Abweichung kennt, ist noch zu entscheiden.
