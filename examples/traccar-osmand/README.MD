# Traccar OsmAnd — robuster Fahrzeug-Tracker mit Store-and-Forward

- Hardware Board-ID: `LILYGO_T_SIM7670G_S3` (ESP32-S3 + SIM7670G, GPS im Modem)
- Pin-Belegung und Power-Sequenzen: siehe [`HARDWARE.md`](HARDWARE.md:1)
- Architektur-Plan: [`plans/traccar-osmand-robust-store-and-forward.md`](plans/traccar-osmand-robust-store-and-forward.md:1)

## Verhalten

Das Gerät bootet **immer** vollständig — auch ohne SIM, ohne Netz, ohne SD-Karte.
`setup()` enthält keine unendlichen Warte-Schleifen:

| Phase | Begrenzung |
|---|---|
| Modem-Poweron + AT-Wait | max 30 s (30 × 1 s), danach 1× PWRKEY-Neupuls, dann trotzdem weiter |
| GPS-Enable | max 5 s (10 × 500 ms) |
| SD-Initialisierung | einmalig, Fallback auf RAM-Puffer |

Danach läuft die Hauptschleife garantiert:

1. **Kontinuierliche GPS-Aufzeichnung** (alle 1 s gepollt, unabhängig vom Netzwerk):
   - jeder gültige Fix wird als CRC16-geprüfter Binär-Record gepuffert
   - Deduplizierung: gleicher Fix (Sekunde + Position) wird nicht doppelt gespeichert
2. **Alle 60 s ein Übertragungsversuch** (auch ohne neue Fixes):
   - Modem-Liveness-Check (`testAT`, 2 s Timeout)
   - Registrierung prüfen: nur `HOME`/`ROAMING` senden; `REG_SMS_ONLY` = kein Datennetz → Daten bleiben gepuffert
   - PDP-Kontext aktivieren (max 2 Versuche)
   - **alle ausstehenden Records** nachsenden, Zeitbudget max 30 s pro Zyklus
   - HTTP 200 → Send-Offset persistieren; Fehler → Record bleibt gepuffert, nächster Zyklus wiederholt
3. **Automatische Erholung**:
   - Modem-Ausfall → PWRKEY-Puls-Reset (GPIO 18, 100 ms) + GPS neu aktivieren, **kein** `esp_restart()` → gepufferte Daten bleiben erhalten
   - SD-Schreibfehler → SD neu initialisieren, nach 3 Fehlern Fallback auf RAM-Puffer
   - Netzwerk-Neuregistrierung erfolgt automatisch im 60-s-Zyklus

## Pufferung (Store-and-Forward)

**Primär: SD-Karte (FAT32)** — überlebt Resets, Kapazität für Tage/Wochen.

- `gps.log`: append-only Binär-Log, Record-Layout (39 Byte):
  ```
  [2 B Magic 'GP'][2 B PayloadLen LE][33 B Payload][2 B CRC16-CCITT]
  ```
  Payload (33 Byte, little endian):
  ```
  [4 B Unix-Zeit][4 B Batt %][1 B Flags (bit0: valid fix)]
  [8 B Lat E7][8 B Lon E7][2 B Speed x100][2 B Alt dm (signed)]
  [2 B HDOP x10][2 B Satelliten-Zahl]
  ```
  ≈ 2,3 KB/h ≈ 56 KB/Tag → eine 8-GB-Karte hält >100 Tage.
- `gps.meta`: Send-Offset (atomar via Rename), überlebt Resets.
- **Kompaktierung**: erst wenn >50 % der Records gesendet und Log ≥ 64 KB → minimiert SD-Verschleiß.
- Korrupte Records (CRC-Fehler) werden beim Senden übersprungen und beim Kompaktieren entfernt.

**Fallback: RAM-Ringpuffer** (128 Records ≈ 5 KB) ohne SD-Karte.
Voll bei Überlauf → ältester Record wird verworfen (nur ohne SD-Karte möglich).

## Konfiguration

In [`traccar-osmand.ino`](traccar-osmand.ino:1):

| Parameter | Standard | Hinweis |
|---|---|---|
| `client_id` | `lilygo-01` | Traccar Device-ID (Kennung) |
| `request_url` | `http://demo3.traccar.org:5055/` | Traccar-Server (HTTP, Port 5055) |
| `NETWORK_APN` | leer (auto) | APN des Operators, falls erforderlich |
| `REPORT_LOCATION_RATE_SECOND` | `60` | Übertragungszyklus in Sekunden |
| `SEND_BUDGET_MS` | `30000` | max. Sendezeit pro Zyklus |
| `MODEM_TEST_TIMEOUT_MS` | `2000` | Timeout des Modem-Liveness-Checks |
| `RAM_RING_SLOTS` | `128` | Größe des RAM-Fallback-Puffers |

Board-Makro: `LILYGO_T_SIM7670G_S3` ist in [`utilities.h`](utilities.h:33) aktiviert.

## Protokoll

- **OsmAnd** (Traccar): https://www.traccar.org/osmand/
- POST-Format:
  `deviceid=<id>&lat=<lat>&lon=<lon>&speed=<speed>&altitude=<alt>&batt=<batt>`
- Der Demo-Server spricht plain **HTTP** auf Port 5055. Der TinyGSM-Fork stellt die
  SIM7672-HTTP-AT-Befehle über die `https_*`-Methoden bereit (intern identisch,
  ohne TLS). Timeouts: `https_set_timeout(10, 10, 10)` (conn/recv/resp je 10 s).

## Power-Tradeoff

- **Original-Demo**: Modem schläft nach jedem Fix (2–3 mA Sleep), aber **keine** Pufferung und blockierender Boot.
- **Diese Version**: Modem bleibt für kontinuierliches GPS wach (höherer Verbrauch),
  dafür lückenlose Aufzeichnung und Store-and-Forward.
  Der ESP32-S3 light-sleeps zwischen GPS-Polls und im 60-s-Wait.
- Erwarteter Verbrauch: deutlich über 2–3 mA, abhängig von GPS- und Modem-Status.

## Build

```bash
# Arduino-IDE / PlatformIO mit Board LILYGO_T_SIM7670G_S3
# oder idf.py build (ESP-IDF-Setup des Projekts)
```

- Flash **nur** nach ausdrücklicher Zustimmung.
- Keine interaktiven Terminalprogramme; `idf.py monitor` nur mit Timeout.
