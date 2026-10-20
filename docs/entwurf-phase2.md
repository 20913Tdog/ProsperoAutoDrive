# Entwurf Phase 2: PS5-Server `prospero-smbd.elf`

Stand: 21.10.2026. Grundlage sind die Spikes S1 bis S7 und die Entscheidungen in [`entscheidungen.md`](entscheidungen.md). Was mit **Vorschlag** markiert ist, bestätigt der Projektverantwortliche noch oder ändert es.

## Ziel

Ein Payload, das nach dem Start ohne weiteres Zutun läuft:

- Es gibt das Dateisystem der PS5 per SMB auf Port 445 frei.
- Das Windows-Programm findet es per UDP 4446. Über den Dienstkanal auf TCP 4445 koppelt es einmal mit dem Programm und meldet ihm danach, dass die PS5 erreichbar ist.

Das Windows-Programm selbst ist Phase 3. In Phase 2 übernehmen Testskripte seine Rolle.

## Was vom Spike bleibt und was neu ist

| Bereich | Spike | Produkt |
|---|---|---|
| Freigabe | neuer Testordner | Root `/`, lesen und schreiben, ohne Schutzliste (D3) |
| Port SMB | 445 (S1) | 445 |
| Port 4445 | HTTP-Steuerport für Tests | **Dienstkanal** (TCP) |
| Port 4446 | UDP-Suche (S7) | UDP-Suche, ohne Uhrzeit |
| Passwort | zufällig, als Benachrichtigung | zufällig, wird beim Koppeln übertragen, nie angezeigt |
| Uhr | Abweichung nur gemessen | Abweichung wird bei allen Zeitstempeln ausgeglichen |
| Server-ID | zufällig je Start | dauerhaft, in der Konfiguration |
| Laufzeit | 15 bis 90 Minuten | unbegrenzt |
| Profil | b | b (1 MiB Socket-Puffer, TCP_NODELAY), bis die Messung zur Wartezeit anderes ergibt |

## Ordner auf der PS5 (Vorschlag)

`/data/prospero-smbd/` gehört dem Payload. Er wird beim ersten Start angelegt:

- `config.ini`: Server-ID, Benutzer, Passwort, gekoppelte PCs, zuletzt gemessene Uhrabweichung
- `log.txt` und `log.1.txt`: Ringlog, je höchstens 1 MiB

Die Testregel betrifft diesen Ordner nicht, denn er ist kein Testordner. **Tests** schreiben weiterhin nur in neu angelegte `_ProsperoAutoDrive_Test_*`-Ordner, auch wenn die Freigabe jetzt `/` ist.

## Dienstkanal TCP 4445

Textzeilen in ASCII, eine Anfrage und eine Antwort je Zeile. Das Programm baut die Verbindung auf und hält sie offen.

| Anfrage des PCs | Antwort der PS5 |
|---|---|
| `HELLO 1 utc_ms=<PC-Zeit> pc=<Name>` | `HELLO 1 server_id=<id> name=<Name> share=<Freigabe> smb_port=445 paired=<0/1>` |
| `PAIR 1 pc=<Name>` | `PAIRED 1 user=<Benutzer> password=<Passwort>` oder `PAIR-REFUSED 1 reason=<Grund>` |
| `PING 1 utc_ms=<PC-Zeit> mac=<Prüfsumme>` | `PONG 1 utc_ms=<Serverzeit> skew_ms=<Abweichung>` |

- **Lebenszeichen:** Das Programm schickt alle 5 s ein `PING`. Bleibt die Antwort 15 s aus und schlägt auch TCP 445 fehl, gilt die PS5 als weg.
- **Uhr:** Die Abweichung kommt aus dem `utc_ms` der `PING`-Zeilen. Übernommen wird sie nur aus einem `PING` mit gültiger Prüfsumme: HMAC-SHA256 über die Zeile, Schlüssel ist das Passwort. So kann kein anderes Gerät die Zeiten verschieben. Gerundet wird auf ganze Sekunden, geändert wird erst ab 2 s Unterschied. Bis zur ersten Messung gilt der Wert aus `config.ini`.
- **Suchen:** Das läuft nicht über den Dienstkanal, sondern über UDP 4446 (siehe unten).
- Nur eine Verbindung je PC. Höchstens 8 Verbindungen gleichzeitig.

## Suche über UDP 4446

Wie in S7 (E3 bestanden): Das Programm fragt zuerst die gespeicherte IP an (Unicast). Antwortet sie nicht, etwa nach einem IP-Wechsel, schickt es einen Rundruf (Broadcast) in jedes eigene Netz. Es bindet keinen festen Port, deshalb erscheint kein Firewall-Fenster.

- Anfrage: `PROSPERO-DISCOVER 1 nonce=<hex>`
- Antwort: `PROSPERO-HERE 1 nonce=<hex> id=<Server-ID> service_port=4445 smb_port=445 name=<Name>`
- Die Server-ID zeigt, ob es dieselbe PS5 ist. Die Uhrzeit läuft nicht mehr hier, sondern über `PING` im Dienstkanal.

## Kopplung (Variante C)

Entschieden ist: Der erste PC, der sich meldet, bekommt Benutzer und Passwort. Die PS5 meldet dann „Prospero AutoDrive: gekoppelt mit \<PC-Name\> (\<IP\>)“. **Vorschlag** für die Einzelheiten:

1. **Erstes Koppeln:** offen, solange noch kein PC gekoppelt ist. Dafür gibt es keine Zeitgrenze, denn der Benutzer richtet das Programm vielleicht erst Tage später ein.
2. **Weiterer PC:** Im Programm auf einem bereits gekoppelten PC wählt man „Weiteren PC koppeln“. Dann ist das Koppeln 10 Minuten lang offen. Ein unbekannter PC kann das nicht auslösen.
3. **Zurücksetzen:** wenn es keinen gekoppelten PC mehr gibt, etwa nach einer Neuinstallation. Dann löscht der Benutzer `config.ini` über einen Dateimanager oder FTP. Beim nächsten Start ist das Payload wieder im Zustand des ersten Starts. Das kommt selten vor.
4. **Passwort:** 24 zufällige Zeichen, das reicht, damit der mitgeschnittene NTLM-Handshake nicht offline zu knacken ist. Benutzer ist `prospero`.

## Freigabe (Vorschlag)

- **Name `PS5`.** Das Programm bekommt den Namen über `HELLO`. Es bleibt damit generisch und kennt den Namen nicht fest.
- **Uhrausgleich im Backend:**
  - beim Melden einer Zeit an Windows die Abweichung abziehen
  - beim Setzen einer Zeit die Abweichung addieren
  - neue Dateien ohne Zeitangabe behalten die Zeit der PS5. Windows sieht sie über den Ausgleich richtig.

## Betrieb

- **Eine Instanz:** Ein neu gestartetes Payload beendet ein laufendes. Dafür meldet es sich über den Dienstkanal mit `QUIT`, das nur von der eigenen Adresse 127.0.0.1 angenommen wird.
- **Ruhemodus:** Bricht das Annehmen von Verbindungen ab, öffnet der Server die Ports neu. Das ist eine äußere Schleife, die auch einen Absturz einer einzelnen Verbindung übersteht.
- **Benachrichtigungen:** nur beim Start, beim Koppeln und bei Fehlern
- **Autostart:** liegt beim Benutzer (PLDMGR, etaHEN). Das Payload braucht dafür nichts Besonderes.
- **Test-Payloads und Produkt** belegen beide Port 4445, es läuft immer nur eins. `tools\Run-Test.cmd` beendet für Produkttests das Produkt über `QUIT`. Das Passwort holt es mit `PAIR`, nicht mehr über `/password`. Testschreibvorgänge bleiben auf neue Testordner beschränkt.

## Arbeitspakete

| Paket | Inhalt | Nachweis |
|---|---|---|
| P2.1 | Grundgerüst: Payload `prospero-smbd`, Ordner, `config.ini`, Ringlog, eine Instanz, Freigabe `/` | E1, E2 |
| P2.2 | Suche (UDP 4446), Dienstkanal und Kopplung, Testskript in der Rolle des Programms | E1, E2, E3 |
| P2.3 | Uhrausgleich im Backend | E1, E2, E3 |
| P2.4 | Wartezeit unter Last: zuerst messen (Liegezeit im Server, Füllstand der Sendeschlange), dann beheben | E3 mit `S4-Check.cmd` |
| P2.5 | Semantik: Freigabemodi, Sperren, Delete-on-Close, Dateiinformationen, Sicherheitsabfrage, Verzeichnisklasse 0x51. Maßstab ist smbtorture. | E1, E2 |
| P2.6 | fsx, Vergleichstester gegen NTFS, 7-Zip, Fuzzing | E2 |

Ein Live-Test (E3) ist nach P2.2, P2.3 und P2.4 nötig, sonst reicht die CI.
