/*
 * Lixie Clock firmware -- PHASE 0 FEASIBILITY SPIKE
 *
 * This is not the real firmware. Its only job is to link every dependency the
 * real firmware will use and report what that costs in flash and RAM, so we
 * find out now rather than at Phase 5 whether it all fits on a Photon 1.
 *
 * Gate: <= 109 KB flash (15% headroom of 128 KB) and >= 12 KB free RAM.
 */

#include <math.h>

#include "neopixel.h"
#include "MQTT.h"

SYSTEM_MODE(SEMI_AUTOMATIC);
SYSTEM_THREAD(ENABLED);

// ---------------------------------------------------------------- display ---
// Sized for the largest build the plan supports (6 digits x 20 LEDs), not the
// 4 digits these two clocks have, so the measurement covers the worst case.
const uint8_t MAX_DIGITS = 6;
const uint16_t LEDS_PER_DIGIT = 20;
const uint16_t PIXEL_COUNT = MAX_DIGITS * LEDS_PER_DIGIT;
const uint8_t PIXEL_PIN = D0;

Adafruit_NeoPixel strip(PIXEL_COUNT, PIXEL_PIN, WS2812B);

// ----------------------------------------------------------------- config ---
// Packed struct straight into the Photon's 2047 bytes of emulated EEPROM.
// Representative of the real thing so the RAM figure is honest.
const uint8_t CFG_VERSION = 1;
const uint8_t MAX_SCHEDULE = 24;

struct ScheduleEntry {
    uint8_t  enabled;
    uint8_t  days_mask;       // bit 0 = Sunday
    uint8_t  anchor;          // 0 clock, 1 sunrise, 2 sunset
    int16_t  minutes;         // minutes-of-day, or offset from the anchor
    uint8_t  mode;            // 0 solid, 1 effect
    uint8_t  effect;
    uint8_t  r, g, b;
    uint8_t  brightness;
    uint8_t  _pad;
};                            // 12 bytes

struct Config {
    uint8_t  version;
    uint8_t  digits;
    char     tz[48];          // POSIX TZ rule, e.g. EST5EDT,M3.2.0,M11.1.0/2
    char     ntp_server[40];
    float    lat, lon;
    char     mqtt_host[40];
    char     mqtt_user[24];
    char     mqtt_pass[24];
    char     web_pass[24];
    uint8_t  mode, effect, r, g, b, brightness;
    ScheduleEntry schedule[MAX_SCHEDULE];
    uint16_t crc;
};

Config cfg;

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c;
}

static void configDefaults() {
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = CFG_VERSION;
    cfg.digits = 4;
    strcpy(cfg.tz, "EST5EDT,M3.2.0,M11.1.0/2");
    strcpy(cfg.ntp_server, "pool.ntp.org");
    cfg.lat = 35.9f; cfg.lon = -78.9f;
    cfg.r = 255; cfg.g = 136; cfg.b = 0;
    cfg.brightness = 80;
}

static void configLoad() {
    EEPROM.get(0, cfg);
    uint16_t want = crc16((const uint8_t *)&cfg, sizeof(cfg) - sizeof(uint16_t));
    if (cfg.version != CFG_VERSION || cfg.crc != want) configDefaults();
}

static void configSave() {
    cfg.crc = crc16((const uint8_t *)&cfg, sizeof(cfg) - sizeof(uint16_t));
    EEPROM.put(0, cfg);
}

// ------------------------------------------------------------------ solar ---
// NOAA sunrise/sunset, written fresh (see the plan's licensing note) rather
// than vendoring the LGPL Sunrise library. Present here mainly to pull in the
// software-float and trig code so it shows up in the flash measurement.
struct SolarTimes { int rise_min; int set_min; bool valid; };

static SolarTimes solarUTC(float lat, float lon, int doy) {
    const float RAD = 3.14159265f / 180.0f;
    float g = 357.529f + 0.98560028f * (doy - 1);
    float lambda = 280.459f + 0.98564736f * (doy - 1) + 1.915f * sinf(g * RAD);
    float decl = asinf(sinf(23.44f * RAD) * sinf(lambda * RAD)) / RAD;
    float B = (doy - 81) * 2.0f * 3.14159265f / 364.0f;
    float eot = 9.87f * sinf(2 * B) - 7.53f * cosf(B) - 1.5f * sinf(B);
    float cosH = cosf(90.833f * RAD) / (cosf(lat * RAD) * cosf(decl * RAD))
               - tanf(lat * RAD) * tanf(decl * RAD);

    SolarTimes out = {0, 0, false};
    if (cosH > 1.0f || cosH < -1.0f) return out;   // polar night / midnight sun
    float H = acosf(cosH) / RAD;
    float noon = 720.0f - 4.0f * lon - eot;
    out.rise_min = (int)(noon - 4.0f * H);
    out.set_min  = (int)(noon + 4.0f * H);
    out.valid = true;
    return out;
}

// -------------------------------------------------------------------- NTP ---
// Own UDP client instead of the ntp-time library: that library burns a software
// timer thread and gives no way to ask how stale the last sync is, which the
// status page and the HA diagnostic sensor both need.
UDP udp;
uint32_t lastNtpSync = 0;
bool ntpEverSynced = false;

static bool ntpSync(const char *server) {
    IPAddress ip = WiFi.resolve(server);
    if (!ip) return false;

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0b11100011;   // LI = 3 (unsync), version 4, mode 3 (client)

    udp.begin(2390);
    udp.sendPacket(pkt, sizeof(pkt), ip, 123);

    uint32_t deadline = millis() + 1500;
    while (millis() < deadline) {
        if (udp.parsePacket() >= 48) {
            udp.read(pkt, 48);
            udp.stop();
            uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16)
                          | ((uint32_t)pkt[42] << 8)  | (uint32_t)pkt[43];
            if (secs < 2208988800UL) return false;
            Time.setTime(secs - 2208988800UL);   // NTP epoch -> Unix epoch
            lastNtpSync = millis();
            ntpEverSynced = true;
            return true;
        }
    }
    udp.stop();
    return false;
}

// ------------------------------------------------------------------- MQTT ---
// 1024-byte buffer: the library defaults to 255, far too small for a Home
// Assistant discovery payload even with abbreviated keys.
void mqttCallback(char *topic, byte *payload, unsigned int length);
MQTT mqtt("192.168.1.2", 1883, 1024, mqttCallback);
volatile uint32_t mqttRxCount = 0;

void mqttCallback(char *topic, byte *payload, unsigned int length) {
    mqttRxCount += length + strlen(topic);
}

// ------------------------------------------------------------------- HTTP ---
TCPServer server(80);
#include "web_assets.h"

static void serveIndex(TCPClient &c) {
    c.print("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
            "Content-Encoding: gzip\r\nAccess-Control-Allow-Origin: *\r\n"
            "Content-Length: ");
    c.print(WEB_INDEX_GZ_LEN);
    c.print("\r\n\r\n");
    for (uint32_t i = 0; i < WEB_INDEX_GZ_LEN; i += 512) {
        uint32_t n = min((uint32_t)512, WEB_INDEX_GZ_LEN - i);
        c.write(WEB_INDEX_GZ + i, n);
    }
}

// Representative of the real /api/state payload, built with snprintf into a
// fixed buffer -- no String, which fragments the heap on Gen 2.
static void serveState(TCPClient &c) {
    char body[512];
    SolarTimes s = solarUTC(cfg.lat, cfg.lon, Time.day());
    int n = snprintf(body, sizeof(body),
        "{\"digits\":%u,\"mode\":%u,\"effect\":%u,\"rgb\":[%u,%u,%u],"
        "\"brightness\":%u,\"tz\":\"%s\",\"ntp\":\"%s\","
        "\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"freemem\":%lu,"
        "\"ntp_age\":%lu,\"mqtt\":%d,\"sunrise\":%d,\"sunset\":%d}",
        cfg.digits, cfg.mode, cfg.effect, cfg.r, cfg.g, cfg.b,
        cfg.brightness, cfg.tz, cfg.ntp_server,
        (int)WiFi.RSSI(), WiFi.localIP().toString().c_str(),
        (unsigned long)(millis() / 1000), (unsigned long)System.freeMemory(),
        (unsigned long)(ntpEverSynced ? (millis() - lastNtpSync) / 1000 : 0),
        mqtt.isConnected() ? 1 : 0, s.rise_min, s.set_min);

    c.print("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\nContent-Length: ");
    c.print(n);
    c.print("\r\n\r\n");
    c.print(body);
}

static void httpTick() {
    TCPClient c = server.available();
    if (!c) return;

    char req[512];
    size_t len = 0;
    uint32_t deadline = millis() + 1000;
    while (c.connected() && millis() < deadline && len < sizeof(req) - 1) {
        if (c.available()) {
            req[len++] = c.read();
            if (len >= 4 && !memcmp(req + len - 4, "\r\n\r\n", 4)) break;
        }
    }
    req[len] = 0;

    if (strstr(req, "GET /api/state")) serveState(c);
    else if (strstr(req, "GET / ")) serveIndex(c);
    else if (strstr(req, "POST /api/config")) {
        configSave();   // links EEPROM.put + CRC into the measurement
        c.print("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
    }
    else c.print("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n");

    c.flush();
    delay(5);
    c.stop();
}

// -------------------------------------------------------------- netwatch ---
retained uint32_t bootCount;
retained uint32_t lastResetReason;

ApplicationWatchdog *wd;

// ------------------------------------------------------------------- main ---
uint32_t lastFrame = 0, lastNtpAttempt = 0, lastMqttAttempt = 0;
uint8_t hue = 0;

void setup() {
    bootCount++;
    lastResetReason = System.resetReason();

    configLoad();

    strip.begin();
    strip.setBrightness(cfg.brightness);
    strip.show();

    WiFi.on();
    WiFi.connect();
    Particle.connect();

    server.begin();
    wd = new ApplicationWatchdog(60000, System.reset, 1536);
}

void loop() {
    uint32_t now = millis();

    // ~50 fps render: rainbow across the strip, representative of the real
    // effect engine's per-frame cost.
    if (now - lastFrame >= 20) {
        lastFrame = now;
        hue++;
        for (uint16_t i = 0; i < PIXEL_COUNT; i++) {
            uint8_t h = hue + (i * 255) / PIXEL_COUNT;
            uint8_t r = (h < 85) ? h * 3 : (h < 170 ? 255 - (h - 85) * 3 : 0);
            uint8_t g = (h < 85) ? 255 - h * 3 : (h < 170 ? (h - 85) * 3 : 0);
            uint8_t b = (h < 170) ? 0 : (h - 170) * 3;
            strip.setPixelColor(i, r, g, b);
        }
        strip.show();
    }

    if (WiFi.ready()) {
        httpTick();

        if (!ntpEverSynced && now - lastNtpAttempt > 30000) {
            lastNtpAttempt = now;
            ntpSync(cfg.ntp_server);
        }

        if (mqtt.isConnected()) {
            mqtt.loop();
        } else if (now - lastMqttAttempt > 15000) {
            lastMqttAttempt = now;
            if (cfg.mqtt_host[0] && mqtt.connect("lixie-spike")) {
                mqtt.subscribe("lixie/spike/set");
                mqtt.publish("lixie/spike/status", "online");
            }
        }
    }

    ApplicationWatchdog::checkin();
}
