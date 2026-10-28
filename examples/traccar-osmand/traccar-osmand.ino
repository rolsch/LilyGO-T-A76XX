/**
 * @file      traccar-osmand.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-03-25
 * @note      Robust vehicle tracking with the OsmAnd protocol (Traccar).
 *
 *            Design goals (see plans/traccar-osmand-robust-store-and-forward.md):
 *            - The board ALWAYS boots fully, even without SIM, without network,
 *              without SD card. setup() contains no unbounded waits.
 *            - GPS is started independently of the mobile network and fixes are
 *              recorded continuously (store-and-forward).
 *            - Every 60 seconds a transmission attempt is made. All network
 *              operations are bounded by timeouts and never block the main loop.
 *            - GPS fixes are buffered on the SD card (FAT32, gps.log) as
 *              CRC-checked binary records with a persistent send offset
 *              (gps.meta). Without an SD card a RAM ring buffer (128 records)
 *              is used as fallback.
 *            - On modem failure the modem is reset via the PWRKEY pulse
 *              sequence (no esp_restart()), so buffered data is preserved.
 *
 *            OsmAnd protocol: https://www.traccar.org/osmand/
 *            Example server (Traccar demo server 3):
 *              Domain: demo3.traccar.org
 *              Port: 5055
 *              Device ID (Kennung): lilygo-01
 */
#define TINY_GSM_RX_BUFFER          1024 // Set RX buffer to 1Kb

// If defined, cancel shallow sleep and use delay to replace shallow sleep
// #define DEBUG_SKETCH

// ------------------------- Timing / robustness -----------------------------
#define REPORT_LOCATION_RATE_SECOND   60      // transmission attempt every 60 s
#define GPS_POLL_INTERVAL_MS          1000    // GPS poll period
#define SEND_BUDGET_MS                30000   // max send time per 60 s cycle
#define MODEM_TEST_TIMEOUT_MS         2000    // modem liveness check timeout
#define MODEM_BOOT_RETRIES            30      // max AT wait at boot (30 s)
#define GPS_ENABLE_RETRIES            10      // max GPS enable retries (5 s)
#define RECOVERY_COOLDOWN_MS          60000   // min interval between modem resets

// ------------------------------ Buffering ----------------------------------
#define RAM_RING_SLOTS                128     // RAM fallback buffer size
#define COMPACT_MIN_SIZE              65536   // compact only files >= 64 KB

#include "utilities.h"
#include <TinyGsmClient.h>
#include <SD.h>

#ifndef MODEM_DTR_PIN
#error "This board does not support modem sleep function"
#endif

TinyGsm modem(SerialAT);

// OsmAnd protocol: device ID (Kennung) and server URL.
// Example values point to the Traccar demo server 3 (see README.MD).
const char *client_id = "lilygo-01";
const char *request_url = "http://demo3.traccar.org:5055/";
// OsmAnd protocol post format:
// http://demo.traccar.org:5055/?deviceid=12345&lat=48.8566&lon=2.3522&speed=15&altitude=35&batt=75
const char *post_format = "deviceid=%s&lat=%.7f&lon=%.7f&speed=%.2f&altitude=%.2f&batt=%u";

// It depends on the operator whether to set up an APN.
//#define NETWORK_APN     "ctlte"             //ctlte: China Telecom

#ifdef NETWORK_APN
String apn = NETWORK_APN;
#else
String apn = "";
#endif
bool use_ipv6_access_point = false; // Whether to use IPv6 to set the access point

// SD card files
#define GPS_LOG_PATH   "/gps.log"
#define GPS_LOG_TMP    "/gps.tmp"
#define GPS_META_PATH  "/gps.meta"
#define GPS_META_TMP   "/gps.meta.tmp"

// Record layout (39 bytes total):
//   [2 B magic 'GP'][2 B payload len LE][33 B payload][2 B CRC16-CCITT over payload]
// Payload (33 bytes, little endian):
//   [4 B unix time][4 B batt %][1 B flags (bit0: valid fix)]
//   [8 B lat E7][8 B lon E7][2 B speed x100][2 B alt dm (signed)]
//   [2 B HDOP x10][2 B satellite count]
#define REC_MAGIC0      'G'
#define REC_MAGIC1      'P'
#define PAYLOAD_LEN     33
#define RECORD_LEN      (2 + 2 + PAYLOAD_LEN + 2)

// ---------------------------------------------------------------------------
// Generic helpers
// ---------------------------------------------------------------------------

void light_sleep_delay(uint32_t ms)
{
#ifdef DEBUG_SKETCH
    delay(ms);
#else
    esp_sleep_enable_timer_wakeup(ms * 1000);
    esp_light_sleep_start();
#endif
}

uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void putU16(uint8_t *b, size_t o, uint16_t v)
{
    b[o] = (uint8_t)(v & 0xFF);
    b[o + 1] = (uint8_t)(v >> 8);
}

static void putU32(uint8_t *b, size_t o, uint32_t v)
{
    b[o] = (uint8_t)(v & 0xFF);
    b[o + 1] = (uint8_t)((v >> 8) & 0xFF);
    b[o + 2] = (uint8_t)((v >> 16) & 0xFF);
    b[o + 3] = (uint8_t)((v >> 24) & 0xFF);
}

static void putI64(uint8_t *b, size_t o, int64_t v)
{
    for (int i = 0; i < 8; i++) {
        b[o + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

static uint16_t getU16(const uint8_t *b, size_t o)
{
    return (uint16_t)(b[o] | ((uint16_t)b[o + 1] << 8));
}

static uint32_t getU32(const uint8_t *b, size_t o)
{
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static int64_t getI64(const uint8_t *b, size_t o)
{
    int64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | b[o + i];
    }
    return v;
}

// GPS civil time (UTC) -> Unix time
static uint32_t gpsToUnix(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second)
{
    if (year < 2000) {
        year = (uint16_t)(year + 2000);
    }
    int y = (int)year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + (int)day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400LL + (int64_t)hour * 3600LL + (int64_t)minute * 60LL + second);
}

// ---------------------------------------------------------------------------
// Record pack / unpack
// ---------------------------------------------------------------------------

static void buildRecord(const uint8_t payload[PAYLOAD_LEN], uint8_t out[RECORD_LEN])
{
    out[0] = REC_MAGIC0;
    out[1] = REC_MAGIC1;
    out[2] = (uint8_t)(PAYLOAD_LEN & 0xFF);
    out[3] = (uint8_t)(PAYLOAD_LEN >> 8);
    memcpy(&out[4], payload, PAYLOAD_LEN);
    uint16_t crc = crc16_ccitt(payload, PAYLOAD_LEN);
    out[4 + PAYLOAD_LEN] = (uint8_t)(crc & 0xFF);
    out[5 + PAYLOAD_LEN] = (uint8_t)(crc >> 8);
}

static bool recordValid(const uint8_t rec[RECORD_LEN])
{
    if (rec[0] != REC_MAGIC0 || rec[1] != REC_MAGIC1) {
        return false;
    }
    if (rec[2] != (uint8_t)(PAYLOAD_LEN & 0xFF) || rec[3] != 0) {
        return false;
    }
    uint16_t crc = crc16_ccitt(&rec[4], PAYLOAD_LEN);
    return (crc & 0xFF) == rec[4 + PAYLOAD_LEN] && (crc >> 8) == rec[5 + PAYLOAD_LEN];
}

static void packPayload(const GPSInfo &info, uint32_t unixTime, uint8_t out[PAYLOAD_LEN])
{
    size_t o = 0;
    putU32(out, o, unixTime);
    o += 4;
    putU32(out, o, 100); // battery percent (fixed, as in the original demo)
    o += 4;
    out[o++] = 0x01; // flags: bit0 = valid fix
    putI64(out, o, (int64_t)lround(info.latitude * 1e7));
    o += 8;
    putI64(out, o, (int64_t)lround(info.longitude * 1e7));
    o += 8;
    putU16(out, o, (uint16_t)constrain((long)(info.speed * 100.0f + 0.5f), 0, 65535));
    o += 2;
    putU16(out, o, (uint16_t)(uint16_t)constrain((long)(info.altitude * 10.0f + 0.5f), -32768, 32767));
    o += 2;
    putU16(out, o, (uint16_t)constrain((long)(info.HDOP * 10.0f + 0.5f), 0, 65535));
    o += 2;
    uint16_t sats = (uint16_t)(info.gps_satellite_num + info.beidou_satellite_num
                               + info.glonass_satellite_num + info.galileo_satellite_num);
    putU16(out, o, sats);
}

// ---------------------------------------------------------------------------
// Buffer: SD card (primary) + RAM ring (fallback)
// ---------------------------------------------------------------------------

static bool sdActive = false;
static uint32_t sendOffset = 0; // byte offset of the next unsent record in gps.log
static int sdFailCount = 0;

// RAM ring buffer (fallback without SD card)
static uint8_t ramRing[RAM_RING_SLOTS][RECORD_LEN];
static uint16_t ramHead = 0; // index of oldest record
static uint16_t ramCount = 0;

static void metaSave()
{
    if (!sdActive) {
        return;
    }
    uint8_t m[12];
    m[0] = 'G';
    m[1] = 'M';
    m[2] = (uint8_t)(sendOffset & 0xFF);
    m[3] = (uint8_t)((sendOffset >> 8) & 0xFF);
    m[4] = (uint8_t)((sendOffset >> 16) & 0xFF);
    m[5] = (uint8_t)((sendOffset >> 24) & 0xFF);
    m[6] = 0;
    m[7] = 0;
    m[8] = 0;
    m[9] = 0;
    uint16_t crc = crc16_ccitt(m, 10);
    m[10] = (uint8_t)(crc & 0xFF);
    m[11] = (uint8_t)(crc >> 8);

    File f = SD.open(GPS_META_TMP, FILE_WRITE);
    if (!f) {
        return;
    }
    f.write(m, 12);
    f.close();
    SD.remove(GPS_META_PATH);
    SD.rename(GPS_META_TMP, GPS_META_PATH);
}

static void metaLoad()
{
    sendOffset = 0;
    File f = SD.open(GPS_META_PATH, FILE_READ);
    if (f) {
        uint8_t m[12];
        if (f.read(m, 12) == 12) {
            uint16_t crc = crc16_ccitt(m, 10);
            if (m[0] == 'G' && m[1] == 'M' && (crc & 0xFF) == m[10] && (crc >> 8) == m[11]) {
                sendOffset = getU32(m, 2);
            }
        }
        f.close();
    }
    // Clamp offset to the actual log size (protects against partial writes)
    File log = SD.open(GPS_LOG_PATH, FILE_READ);
    if (log) {
        if (sendOffset > log.size()) {
            sendOffset = 0;
        }
        log.close();
    } else {
        sendOffset = 0;
    }
    Serial.printf("Meta loaded: sendOffset=%lu\n", (unsigned long)sendOffset);
}

static void sdFailure()
{
    sdFailCount++;
    Serial.printf("SD write failure (%d)\n", sdFailCount);
    if (sdFailCount >= 3) {
        sdActive = false;
        Serial.println("SD disabled, falling back to RAM buffer");
        return;
    }
    // Try to re-initialize the SD card
    SD.end();
    delay(100);
    if (SD.begin(BOARD_SD_CS_PIN)) {
        sdFailCount = 0;
        Serial.println("SD re-initialized");
    } else {
        sdActive = false;
        Serial.println("SD re-init failed, falling back to RAM buffer");
    }
}

static bool bufferAppendRam(const uint8_t payload[PAYLOAD_LEN])
{
    uint8_t rec[RECORD_LEN];
    buildRecord(payload, rec);
    if (ramCount == RAM_RING_SLOTS) {
        // Buffer full: drop the oldest record (worst case without SD card)
        ramHead = (uint16_t)((ramHead + 1) % RAM_RING_SLOTS);
        ramCount--;
    }
    uint16_t tail = (uint16_t)((ramHead + ramCount) % RAM_RING_SLOTS);
    memcpy(ramRing[tail], rec, RECORD_LEN);
    ramCount++;
    return true;
}

static bool bufferAppend(const uint8_t payload[PAYLOAD_LEN])
{
    if (sdActive) {
        File f = SD.open(GPS_LOG_PATH, FILE_WRITE);
        if (f) {
            uint8_t rec[RECORD_LEN];
            buildRecord(payload, rec);
            bool ok = (f.write(rec, RECORD_LEN) == RECORD_LEN);
            f.close();
            if (ok) {
                return true;
            }
            sdFailure();
        } else {
            sdFailure();
        }
    }
    return bufferAppendRam(payload);
}

// Peek the next unsent record WITHOUT consuming it.
static bool bufferPeekNext(uint8_t *outRec)
{
    if (sdActive) {
        for (int guard = 0; guard < 1024; guard++) {
            File f = SD.open(GPS_LOG_PATH, FILE_READ);
            if (!f) {
                return false;
            }
            if (f.size() <= sendOffset) {
                f.close();
                return false;
            }
            bool ok = false;
            if (f.seek(sendOffset)) {
                uint8_t rec[RECORD_LEN];
                if (f.read(rec, RECORD_LEN) == (int)RECORD_LEN && recordValid(rec)) {
                    memcpy(outRec, rec, RECORD_LEN);
                    ok = true;
                }
            }
            f.close();
            if (ok) {
                return true;
            }
            // Corrupted record: skip it
            sendOffset += RECORD_LEN;
            metaSave();
        }
        return false;
    }
    if (ramCount == 0) {
        return false;
    }
    memcpy(outRec, ramRing[ramHead], RECORD_LEN);
    return true;
}

// Consume the record that was just sent successfully.
static void bufferMarkSent()
{
    if (sdActive) {
        sendOffset += RECORD_LEN;
        metaSave();
    } else {
        ramHead = (uint16_t)((ramHead + 1) % RAM_RING_SLOTS);
        ramCount--;
    }
}

static bool bufferHasPending()
{
    if (sdActive) {
        File f = SD.open(GPS_LOG_PATH, FILE_READ);
        if (!f) {
            return false;
        }
        bool pending = (f.size() > sendOffset);
        f.close();
        return pending;
    }
    return ramCount > 0;
}

// Compact gps.log once more than 50 % of it has been sent (limits SD wear).
static void maybeCompact()
{
    if (!sdActive) {
        return;
    }
    File probe = SD.open(GPS_LOG_PATH, FILE_READ);
    if (!probe) {
        return;
    }
    uint32_t size = probe.size();
    probe.close();
    if (size < COMPACT_MIN_SIZE) {
        return;
    }
    if (sendOffset * 2 < size) {
        return; // less than 50 % sent
    }
    Serial.printf("Compacting gps.log (%lu bytes, offset %lu)\n", (unsigned long)size, (unsigned long)sendOffset);

    File src = SD.open(GPS_LOG_PATH, FILE_READ);
    File dst = SD.open(GPS_LOG_TMP, FILE_WRITE);
    if (!src || !dst) {
        if (src) src.close();
        if (dst) dst.close();
        return;
    }
    if (src.seek(sendOffset)) {
        uint8_t rec[RECORD_LEN];
        while (src.available()) {
            if (src.read(rec, RECORD_LEN) != (int)RECORD_LEN) {
                break;
            }
            if (recordValid(rec)) {
                dst.write(rec, RECORD_LEN);
            }
        }
    }
    src.close();
    dst.close();
    SD.remove(GPS_LOG_PATH);
    SD.rename(GPS_LOG_TMP, GPS_LOG_PATH);
    sendOffset = 0;
    metaSave();
    Serial.println("Compaction done");
}

// ---------------------------------------------------------------------------
// Modem management (bounded, non-blocking)
// ---------------------------------------------------------------------------

static void pwrkeyPulse()
{
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(MODEM_POWERON_PULSE_WIDTH_MS);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
}

static void enableGps()
{
    Serial.println("Enabling GPS/GNSS...");
    bool ok = false;
    for (int i = 0; i < GPS_ENABLE_RETRIES; i++) {
        if (modem.enableGPS(MODEM_GPS_ENABLE_GPIO, MODEM_GPS_ENABLE_LEVEL)) {
            ok = true;
            break;
        }
        delay(500);
    }
    if (ok) {
        Serial.println("GPS enabled");
#if defined(TINY_GSM_MODEM_A7670) || defined(TINY_GSM_MODEM_A7608)
        modem.setGPSMode(GNSS_MODE_GPS_BDS_GALILEO_SBAS_QZSS);
#elif defined(TINY_GSM_MODEM_SIM7670G)
        modem.setGPSMode(GNSS_MODE_GPS_GLONASS_GALILEO_BDS);
#elif defined(TINY_GSM_MODEM_SIM7600)
        modem.setGPSMode(GNSS_MODE_ALL);
#elif defined(TINY_GSM_MODEM_SIM7000SSL) || defined(TINY_GSM_MODEM_SIM7000)
        modem.setGPSMode(GNSS_MODE_ALL);
#endif
    } else {
        Serial.println("WARNING: GPS enable failed, will retry in loop");
    }
}

static void bootModem()
{
    // Set modem reset pin, reset modem
#ifdef MODEM_RESET_PIN
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
    delay(2600);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    gpio_hold_en((gpio_num_t)MODEM_RESET_PIN);
    gpio_deep_sleep_hold_en();
#endif

    // Pull down DTR to ensure the modem is not in sleep state
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);

    // Turn on the modem (PWRKEY pulse sequence, see HARDWARE.md)
    pwrkeyPulse();

    Serial.println("Waiting for modem AT (max 30 s)...");
    bool up = false;
    for (int i = 0; i < MODEM_BOOT_RETRIES; i++) {
        if (modem.testAT(1000)) {
            up = true;
            break;
        }
    }
    if (!up) {
        Serial.println("Modem not responding, one PWRKEY re-pulse...");
        pwrkeyPulse();
        for (int i = 0; i < 10; i++) {
            if (modem.testAT(1000)) {
                up = true;
                break;
            }
        }
    }
    if (up) {
        String name = modem.getModemName();
        Serial.print("Modem: ");
        Serial.println(name);
        enableGps();
    } else {
        Serial.println("WARNING: modem offline at boot, loop() will keep trying");
    }
}

static uint32_t lastRecoveryAttempt = 0;

static void recoverModem()
{
    if (millis() - lastRecoveryAttempt < RECOVERY_COOLDOWN_MS) {
        return; // avoid hammering the modem
    }
    lastRecoveryAttempt = millis();
    Serial.println("Modem unresponsive - resetting via PWRKEY pulse");
    pwrkeyPulse();
    bool up = false;
    for (int i = 0; i < MODEM_BOOT_RETRIES; i++) {
        if (modem.testAT(1000)) {
            up = true;
            break;
        }
    }
    if (up) {
        enableGps();
        Serial.println("Modem recovered");
    } else {
        Serial.println("Modem still offline, will retry next cycle");
    }
}

// ---------------------------------------------------------------------------
// GPS recording
// ---------------------------------------------------------------------------

static bool hasLastFix = false;
static uint32_t lastFixTime = 0;
static int64_t lastFixLat = 0;
static int64_t lastFixLon = 0;

// Skip duplicate fixes (same second + same position) to avoid log spam.
static bool isDuplicateFix(const GPSInfo &info)
{
    uint32_t t = gpsToUnix(info.year, info.month, info.day, info.hour, info.minute, info.second);
    int64_t lat = (int64_t)lround(info.latitude * 1e7);
    int64_t lon = (int64_t)lround(info.longitude * 1e7);
    if (hasLastFix && t == lastFixTime && lat == lastFixLat && lon == lastFixLon) {
        return true;
    }
    hasLastFix = true;
    lastFixTime = t;
    lastFixLat = lat;
    lastFixLon = lon;
    return false;
}

static void recordGps(const GPSInfo &info)
{
    uint32_t t = gpsToUnix(info.year, info.month, info.day, info.hour, info.minute, info.second);
    uint8_t payload[PAYLOAD_LEN];
    packPayload(info, t, payload);
    bufferAppend(payload);
    Serial.printf("Recorded fix: %.6f,%.6f speed=%.2f alt=%.1f sats=%u\n",
                  info.latitude, info.longitude, info.speed, info.altitude,
                  (unsigned)(info.gps_satellite_num + info.beidou_satellite_num
                             + info.glonass_satellite_num + info.galileo_satellite_num));
}

// ---------------------------------------------------------------------------
// Transmission (store-and-forward, bounded by SEND_BUDGET_MS)
// ---------------------------------------------------------------------------

static void sendCycle()
{
    // 1. Modem liveness (bounded)
    if (!modem.testAT(MODEM_TEST_TIMEOUT_MS)) {
        recoverModem();
        return;
    }

    // 2. Network registration: only HOME/ROAMING can send data.
    //    REG_SMS_ONLY (SIM7670G) means "no data network" -> stay buffered.
    RegStatus st = modem.getRegistrationStatus();
    if (st != REG_OK_HOME && st != REG_OK_ROAMING) {
        Serial.printf("No data network (reg=%d), data stays buffered\n", (int)st);
        return;
    }

    // 3. PDP context (max 2 attempts)
    if (!modem.setNetworkActive(apn, use_ipv6_access_point)) {
        delay(1000);
        if (!modem.setNetworkActive(apn, use_ipv6_access_point)) {
            Serial.println("PDP activation failed, retry next cycle");
            return;
        }
    }

    // 4. Send all pending records within the time budget
    if (!bufferHasPending()) {
        return; // nothing to send
    }

    modem.https_begin();
    modem.https_set_timeout(10, 10, 10); // conn=10s, recv=10s, resp=10s
    if (!modem.https_set_url(request_url)) {
        Serial.println("Failed to set the URL. Please check the validity of the URL!");
        modem.https_end();
        return;
    }
    modem.https_set_user_agent("TinyGSM/LilyGo-A76XX");

    uint32_t start = millis();
    int sent = 0;
    while (millis() - start < SEND_BUDGET_MS) {
        uint8_t rec[RECORD_LEN];
        if (!bufferPeekNext(rec)) {
            break; // all sent
        }

        // Unpack payload (payload starts at rec[4])
        uint32_t batt = getU32(rec, 8);
        int64_t latE7 = getI64(rec, 13);
        int64_t lonE7 = getI64(rec, 21);
        float lat = (float)latE7 / 1e7f;
        float lon = (float)lonE7 / 1e7f;
        float speed = (float)getU16(rec, 29) / 100.0f;
        float alt = (float)(int16_t)getU16(rec, 31) / 10.0f;

        char post_buffer[128];
        snprintf(post_buffer, sizeof(post_buffer), post_format, client_id, lat, lon, speed, alt, batt);

        int httpCode = modem.https_post(post_buffer);
        if (httpCode == 200) {
            bufferMarkSent();
            sent++;
        } else {
            Serial.printf("HTTP post failed! error code = %d, record kept, retry next cycle\n", httpCode);
            break;
        }
    }
    modem.https_end();

    if (sent > 0) {
        Serial.printf("Sent %d buffered record(s)\n", sent);
    }
    maybeCompact();
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

static uint32_t lastSendAttempt = 0;

void setup()
{
    Serial.begin(115200); // Set console baud rate
    Serial.println("Start Sketch (robust store-and-forward)");

#ifdef BOARD_LED_PIN
    pinMode(BOARD_LED_PIN, OUTPUT);
    digitalWrite(BOARD_LED_PIN, LOW);
#endif

    // SD card (optional - RAM ring buffer is the fallback)
    if (SD.begin(BOARD_SD_CS_PIN)) {
        sdActive = true;
        metaLoad();
        Serial.println("SD card active, buffering to gps.log");
    } else {
        sdActive = false;
        Serial.printf("SD card not available, using RAM buffer (%d records)\n", RAM_RING_SLOTS);
    }

    // Modem power-on + bounded AT wait + bounded GPS enable.
    // NOTE: no SIM/network wait here on purpose - the main loop must always start.
    bootModem();

    Serial.println("setup() done, entering main loop");
}

void loop()
{
    // 1. Modem liveness + continuous GPS recording (independent of network)
    if (modem.testAT(MODEM_TEST_TIMEOUT_MS)) {
        GPSInfo info;
        if (modem.getGPS_Ex(info) && info.isFix) {
            if (!isDuplicateFix(info)) {
                recordGps(info);
            }
        }
    } else {
        recoverModem(); // bounded, with cooldown
    }

    // 2. Transmission attempt every 60 s (even without new fixes)
    if (millis() - lastSendAttempt >= (uint32_t)REPORT_LOCATION_RATE_SECOND * 1000UL) {
        lastSendAttempt = millis();
        sendCycle();
    }

    // 3. Sleep until the next poll (ESP light sleep, modem stays awake for GPS)
    light_sleep_delay(GPS_POLL_INTERVAL_MS);
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "No correct definition detected, please copy all the [lib directories](https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX/tree/main/lib) to the arduino libraries directory, see README"
#endif
