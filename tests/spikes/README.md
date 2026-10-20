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
