# Machbarkeits-Spikes

Die Spikes S1–S7 klären vor jedem Produktcode, ob die Architektur „natives SMB“ trägt (Gate G1). Jeder Spike nennt seine Evidenzebene:

- **E1:** CI unter Linux
- **E2:** CI mit echtem Windows-Client
- **E3:** echte PS5

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

- `prospero-s2-smbserver.elf` legt einen **neuen** Testordner `/data/_ProsperoAutoDrive_Test_<Zeit>-<Zufall>` an und füllt ihn mit festen Testdateien, zusammen etwa 65 MB (Liste in `ps5/spikes/s2-smbserver/testdata.c`).
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
   Auf der PS5 erscheint `Prospero S2 bereit: Freigabe s2, Benutzer prospero, Passwort xxxx-xxxx-xxxx (15 min)`.
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

**Notiert für S3:** Die Uhr der PS5 und die des PCs liegen rund 50 Minuten auseinander. Das zeigen die Dateizeiten der PS5 im Vergleich zur robocopy-Endzeit. Für Zeitstempel beim Schreiben ist das wichtig.

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
   Der FTP-Port ist optional (Standard 1337). Er gilt für einen FTP-Server, der auf der PS5 schon läuft und die WLAN-Referenz liefert. Das Skript trennt zuerst alle bestehenden Verbindungen zur PS5. Windows lässt pro Server nur einen Benutzernamen zu (Systemfehler 1219).

   Das dauert einige Minuten. Am PC und auf der PS5 in der Zeit möglichst nichts im Netz laden, keine Downloads und kein Streaming.
4. Die komplette Ausgabe bis `S2-SPEED: FERTIG` zurückmelden.

### Auswertung

| Beobachtung | Bedeutung | Konsequenz |
|---|---|---|
| b deutlich schneller als a | Die Socket-Puffer bremsen | Profil b übernehmen |
| c deutlich schneller als b, `cpu_user_ms` nahe `read_window_ms` | Die Signatur rechnet zu langsam | schnelle AES-CMAC (AES-NI) in libsmb2 einbauen, als Beitrag für upstream |
| a, b und c ähnlich langsam wie die FTP-Referenz | Das WLAN begrenzt | Vergleich mit ftpsrv in S4 entscheidet |
| alle deutlich langsamer als die FTP-Referenz, CPU niedrig | Wartezeiten im Protokollablauf | Ablauf genauer untersuchen |
