# Entwurf Phase 3: Windows-Programm `ProsperoAutoDrive.exe`

Stand: 24.10.2026, **Vorschlag, noch nicht bestätigt**. Grundlage sind [`entscheidungen.md`](entscheidungen.md), vor allem „Sofort“ für Verbinden und Trennen, und der Dienstkanal aus dem [Entwurf Phase 2](entwurf-phase2.md).

## Ziel

Eine einzelne exe, die einmal eingerichtet wird und danach nie wieder angefasst werden muss:

- Ist die PS5 erreichbar, erscheint nach höchstens 3 s das Laufwerk `P:`.
- Ist sie weg, verschwindet es nach höchstens 5 s am Kabel bzw. 15 s im WLAN, bei ordentlichem Ende des Payloads nach unter 1 s.
- Im Leerlauf kostet das auf PC und PS5 unter 0,1 % eines Kerns (P30 gemessen: PC 0,038 %, PS5 0,045–0,080 %).
- Immer ein Tray-Symbol, Benachrichtigung bei Verbinden, Trennen und Problemen.
- Kein Admin, kein Treiber, keine Fremdsoftware, kein PowerShell. Das Programm bleibt **generisch**: Es kennt nur „einen Server mit Dienstkanal und SMB-Freigabe“, nichts PS5-Spezifisches. Name, Freigabe und Ports kommen aus `HELLO`.

## Zustände

```
            ┌──────────────── Fehler / Server weg ────────────────┐
            v                                                      │
 SUCHEN ──TCP 4445 ok, HELLO, Server-ID passt──> KOPPELN? ──> VERBINDEN ──> ONLINE
   ^  alle 2 s Versuch an die gespeicherte IP      (nur einmal)   (P: anlegen)   │ PING jede s
   │  Rundruf nur auf Wunsch (Tray-Menü)                                         │
   └──────────── P: entfernt <── PRÜFEN (TCP 445) <── Lebenszeichen fehlt ───────┘
```

| Zustand | Was passiert | Weiter |
|---|---|---|
| **Suchen** | Alle 2 s ein Verbindungsversuch an die gespeicherte IP, 1 s Zeitlimit. **Kein automatischer Rundruf** (Entscheidung vom 24.10.2026): Das Tray-Symbol zeigt „nicht erreichbar, zuletzt gesehen am …“, der Rundruf läuft nur über „PS5 im Netz suchen (30 s)“ im Tray-Menü und einmal bei der Einrichtung. | Dienstkanal steht und Server-ID passt |
| **Koppeln** | Nur beim ersten Mal, wenn keine Zugangsdaten gespeichert sind: `PAIR`, Zugangsdaten in die Anmeldeinformationsverwaltung | Verbinden |
| **Verbinden** | `WNetAddConnection2` mit `CONNECT_TEMPORARY` auf `P:` | Online, oder bei Fehler Hinweis und Suchen mit Pause |
| **Online** | `PING` mit Prüfsumme jede Sekunde, enthält die PC-Uhrzeit | Antwort fehlt: Prüfen |
| **Prüfen** | Nach 3 verpassten Antworten (Kabel, `link=wired`) bzw. 12 (sonst): Verbindungsversuch auf TCP 445, 1 s Zeitlimit. Wird der Dienstkanal von der PS5 geschlossen, sofort prüfen. | 445 antwortet: weiter Online, Kanal neu aufbauen. Sonst `P:` entfernen, Suchen |

Rechnung Kabel: 3 s verpasst + höchstens 1 s Test + Trennen ≈ 4–5 s. Ordentliches Ende: Das Payload schließt den Kanal, die Gegenstelle merkt das sofort, der Test auf 445 wird sofort abgewiesen, also unter 1 s.

**Weitere Auslöser:**
- PC wacht aus dem Standby auf, Netzwerk wechselt (`NotifyIpInterfaceChange`): sofort Prüfen bzw. Suchen, nicht erst nach dem Takt.
- Abmelden, Herunterfahren: `P:` entfernen.

## Entspannt für beide Maschinen

- Ein Thread wartet mit `WaitForMultipleObjects` auf den Socket (`WSAEventSelect`), einen Wartezeit-Timer und Systemereignisse. Kein Abfragen in einer Schleife, kein Aufwachen öfter als einmal je Sekunde.
- Ein `PING` sind etwa 150 Bytes hin und 100 zurück.
- Ist die PS5 aus, schickt der PC alle 2 s einen Verbindungsversuch ins eigene Netz. Das ist ein einzelnes Paket und belastet nichts.

## Laufwerksbuchstabe

- Standard `P:`. Ist `P:` belegt (fremdes Mapping, USB-Stick), wird es **nicht** angefasst.
- **Vorschlag:** Dann nimmt das Programm den nächsten freien Buchstaben rückwärts ab `O:` und meldet das einmal. Alternative: gar kein Laufwerk und ein Hinweis.

## Einrichtung, Autostart, Deinstallation

- **Erster Start** (Doppelklick auf die heruntergeladene exe):
  1. Das Programm kopiert sich nach `%LOCALAPPDATA%\Programs\ProsperoAutoDrive\` und startet von dort.
  2. Es trägt sich in den Autostart des Benutzers ein (`HKCU\...\Run`), ohne UAC.
  3. Es sucht die PS5: erst Rundruf, dann Eingabe der IP. Danach koppelt es und speichert IP, Server-ID und Zugangsdaten.
- **Tray-Menü:** Status in der ersten Zeile (z. B. „PS5 verbunden als P:, Kabel“ oder „PS5 nicht erreichbar, zuletzt gesehen am …“), „Laufwerk öffnen“, „PS5 im Netz suchen (30 s)“, „Weiteren PC koppeln“, „Mit Windows starten“ (an/aus), „Deinstallieren“, „Beenden“.
- **Deinstallieren** aus dem Tray-Menü: `P:` entfernen, Autostart-Eintrag, Zugangsdaten und Einstellungen löschen, danach den Programmordner. Die laufende exe kann sich nicht selbst löschen. Das übernimmt ein kurzer `cmd.exe`-Aufruf, der wartet, bis das Programm beendet ist (kein PowerShell).
- **Einstellungen:** `%LOCALAPPDATA%\ProsperoAutoDrive\settings.ini` (IP, Server-ID, Buchstabe), Ringlog daneben. Das Passwort steht nur in der Anmeldeinformationsverwaltung.

## Benachrichtigungen

Über das Tray-Symbol (`Shell_NotifyIcon`), ohne Fremdbibliothek:

- „PS5 verbunden als P:“, „PS5 getrennt“
- Probleme: Anmeldung abgelehnt, `P:` belegt, Koppeln abgelehnt. Jeweils einmal, nicht bei jedem Versuch. Ein falsches Passwort führt nicht zu einem Sturm von Wiederholungen: Danach wird nur noch alle 60 s versucht.
- Kurze Aussetzer im WLAN unterhalb der Trennzeit melden nichts.

## Aufbau des Codes

| Teil | Inhalt | Test |
|---|---|---|
| `windows/app/core/` | Zustandsautomat ohne Windows-Aufrufe: Eingaben sind Ereignisse und die Uhrzeit, Ausgaben sind Aktionen („verbinde Kanal“, „P: anlegen“, „melde …“) | E1: Unit-Tests mit simulierter Uhr im Preset `host`, auch unter Linux |
| `windows/app/` | Win32-Schicht: Sockets, `WNet*`, Anmeldeinformationen, Tray, Autostart | E2 |
| `windows/common/` | Dienstkanal und Suche, schon da (vom Testprogramm) | E1/E2 |

## Tests

- **E1:** Zustandsautomat: Alle Zeitgrenzen (3 s, 5 s, 15 s, < 1 s) mit simulierter Uhr, Aussetzer kürzer als die Trennzeit trennen nicht, falsches Passwort ohne Sturm.
- **E2:** Echter Windows-Client gegen den Server in WSL wie bisher. Server an: Laufwerk da. Server beendet: Laufwerk weg. Server wieder an: Laufwerk wieder da. Fremdes Mapping bleibt unberührt.
  - **Offener technischer Punkt:** In der CI läuft der Server nicht auf Port 445, sondern auf 4450 (Windows belegt 445 selbst). `net use` kann einen anderen Port (`/TCPPORT`), `WNetAddConnection2` nicht. Ich prüfe, ob `WNetAddConnection4` das über seine Optionen kann. Sonst bekommt das Programm für die CI einen Port aus `HELLO` (`smb_port`). Das wäre auch für Nutzer mit anderem Port sinnvoll.
- **E3:** bei dir: Einrichtung, 10 × PS5 aus und an, Zeiten aus dem Log des Programms, CPU im Leerlauf.

## Arbeitspakete

| Paket | Inhalt | Nachweis |
|---|---|---|
| P3.1 | Zustandsautomat mit Unit-Tests | E1 |
| P3.2 | Win32-Schicht: Kanal, Lebenszeichen, `P:` anlegen und entfernen, Log. Noch ohne Tray, über Kommandozeile startbar | E2, E3 |
| P3.3 | Tray, Benachrichtigungen, Einrichtung, Autostart, Deinstallation | E2, E3 |
| P3.4 | Abnahme Phase 3: Zeiten und CPU, 10 Zyklen | E3, dann Release 0.1.0 |
