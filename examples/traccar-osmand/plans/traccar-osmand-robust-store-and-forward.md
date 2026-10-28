# Plan: Robuster Traccar-OsmAnd-Tracker mit Store-and-Forward

**Board:** LilyGO T-SIM767XG-S3 (ESP32-S3 + SIM7670G, GPS im Modem)
**Ziel:** Gerät bootet immer vollständig, GPS läuft unabhängig vom Netzwerk,
keine GPS-Datenverluste über Stunden/Tage ohne Mobilfunk, automatische Erholung.

## 1. Architektur-Entscheidung

**Vorschlag B — SD-Log + Store-and-Forward** (aus 3 Vorschlägen gewählt)

| Kriterium | A: RAM-Ring | **B: SD-Log (gewählt)** | C: Hybrid-WAL |
|---|---|---|---|
| Datensicherheit | niedrig (Reset = Verlust) | **hoch (überlebt Reset, Tage/Wochen)** | höchst (crash-safe mid-write) |
| RAM-Verbrauch | ~16 KB | ~2 KB | ~4 KB |
| SD-Verschleiß | keine | niedrig (1 Write/min + seltene Kompaktierung) | niedrig–mittel |
| Wiederanlauf nach Reset | kein | vollständig (Meta + Log) | vollständig + partiell |
| Aufwand | gering | mittel | hoch |

Begründung: erfüllt die bevorzugte Store-and-Forward-Architektur,
degradiert elegant auf RAM-Ringpuffer (Vorschlag A) ohne SD-Karte.

## 2. Record-Format (gps.log, append-only, FAT32)

```
[2 B Magic 'GP'][2 B PayloadLen LE][N B Payload][2 B CRC16-CCITT über Payload]
```

Payload (binär, kompakt):
```
[4 B Unix-Timestamp][4 B Batt %][1 B Flags (bit0: valid fix)][8 B Lat E7 LE]
[8 B Lon E7 LE][2 B Speed cm/s LE][2 B Alt dm LE][2 B HDOP x10 LE][2 B SatNum]
```
= 40 B Payload, 46 B Gesamt pro Record.
46 B/min ≈ 2,7 KB/h ≈ 66 KB/Tag → 8 GB-Karte hält >100 Tage.

**gps.meta** (kleine Datei, atomar via Rename):
```
[2 B Magic 'GM'][4 B SendOffset LE][4 B RecordCount LE][2 B CRC16]
```

## 3. Blockfreier Boot (Pflicht 1, 2, 8)

`setup()` enthält **keine** unendlichen Schleifen:

1. Serial, LED, SD-Init (begrenzt, Fallback RAM-Modus)
2. Modem-Poweron-Sequenz (PWRKEY GPIO 18, 100 ms Puls)
3. `modem.testAT()` max 30 s Retry (30 × 1 s), bei Fehlschlag: 1× PWRKEY-Neupuls, dann trotzdem weiter
4. `modem.enableGPS(MODEM_GPS_ENABLE_GPIO, MODEM_GPS_ENABLE_LEVEL)` max 5 s Retry
5. `modem.setGPSMode(GNSS_MODE_GPS_GLONASS_GALILEO_BDS)` (SIM7670G, kein AGPS)
6. HTTP-Timeouts via `modem.https_set_timeout(10, 10, 10)` (conn/recv/resp je 10 s);
   AT-Operationen nutzen die eingebauten Timeouts des TinyGSM-Forks
7. `loop()` startet **immer** — auch ohne SIM, ohne Netz, ohne SD

SIM-Status, Registrierung, APN, `setNetworkActive` werden NUR im 60-s-Zyklus geprüft.

## 4. Hauptschleife (Pflicht 3, 4, 5, 7)

Zustandsmaschine in `loop()`:

1. **GPS-Aufzeichnung (kontinuierlich):** `modem.getGPS_Ex()` mit AT-Timeout;
   gültiger Fix (isFix) → Deduplizierung (gleiche Sekunde + Lat/Lon = überspringen)
   → Record in gps.log appenden (oder RAM-Ring ohne SD).
   Kein Fix → light-sleep 1 s, weiter.
2. **60-s-Übertragungszyklus** (unabhängig von neuen Fixes):
   - `modem.testAT(2000)` → Modem tot? → PWRKEY-Puls-Reset + GPS neu (kein `esp_restart()`)
   - `getRegistrationStatus()`: nur HOME/ROAMING = sendbar;
     `REG_SMS_ONLY` = kein Datennetz (board-spezifisch, HARDWARE.md)
   - `setNetworkActive(apn)` max 2 Versuche
   - **Batch-Nachsenden:** alle ausstehenden Records ab SendOffset,
     HTTP-POST pro Record, Zeitbudget max 30 s pro Zyklus
   - HTTP 200 → SendOffset fortschreiben (persistieren nach jedem Record)
   - HTTP-Fehler → Offset halten, nächster Zyklus wiederholt (Store-and-Forward)
   - Kompaktierung: wenn >50 % der Records gesendet → Log neu schreiben, Offset = 0
3. **SD-Fehler:** Schreibfehler → SD neu initialisieren; 3× Fehlschlag → RAM-Modus

## 5. Timeouts (Pflicht 6)

| Operation | Timeout |
|---|---|
| `testAT()` Watchdog | 2 s |
| `https_post()` (via https_set_timeout) | 10 s (conn/recv/resp) |
| Send-Budget pro 60-s-Zyklus | 30 s |
| Modem-Poweron-Wait | 30 s |
| GPS-Enable | 5 s |

## 6. Dateien

| Datei | Aktion |
|---|---|
| `utilities.h` | `LILYGO_T_SIM7670G_S3` aktivieren (aktuell kommentiert) |
| `traccar-osmand.ino` | Neu schreiben: blockfreier Boot, Puffer, Zustandsmaschine |
| `README.MD` | Aktualisieren: neues Verhalten, Record-Format, Konfiguration, Power-Tradeoff |

## 7. Power-Tradeoff (README)

- Modem bleibt für kontinuierliches GPS wach (statt 2–3 mA Sleep nach jedem Fix)
- ESP light-sleeps zwischen GPS-Polls und im 60-s-Wait
- Erwartet: deutlich höherer Verbrauch als Original-Demo, dafür lückenlose Aufzeichnung

## 8. Verifikation

- `idf.py build` / Arduino-Build, max 3 Fix-Zyklen
- Kein Flash ohne ausdrückliche Zustimmung
- Keine interaktiven Befehle, kein `idf.py monitor` ohne Timeout
