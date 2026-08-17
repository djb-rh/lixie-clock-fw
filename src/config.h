#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Persisted settings. Lives in the Photon 1's 2047 bytes of emulated EEPROM as a
// packed struct with a CRC, because Gen 2 has no filesystem.
//
// Deliberately free of any Particle dependency so the layout and CRC invariants
// can be checked on the host -- see tests/config_test.cpp.
//
// Adding a field: bump CFG_VERSION and handle the old layout in configLoad(),
// or accept that existing clocks fall back to defaults on the next boot.

const uint8_t CFG_VERSION = 2;
const uint8_t MAX_SCHEDULE = 24;
const uint8_t MAX_DIGITS = 6;
const uint16_t LEDS_PER_DIGIT = 20;

enum : uint8_t { ANCHOR_CLOCK = 0, ANCHOR_SUNRISE = 1, ANCHOR_SUNSET = 2 };
enum : uint8_t { MODE_SOLID = 0, MODE_EFFECT = 1 };

struct ScheduleEntry {
    uint8_t enabled;
    uint8_t days_mask;     // bit 0 = Sunday
    uint8_t anchor;        // ANCHOR_*
    int16_t minutes;       // minutes-of-day, or signed offset from the anchor
    uint8_t mode;          // MODE_*
    uint8_t effect;
    uint8_t r, g, b;
    uint8_t brightness;    // percent
    uint8_t _pad;
};

// The version 1 layout, kept only so existing clocks can be migrated rather
// than reset. Never change this -- it describes what is already in EEPROM out
// in the world.
struct ConfigV1 {
    uint8_t version;
    uint8_t digits;
    uint8_t hour_format;
    uint8_t blank_hour_zero;
    char tz[48];
    char ntp_server[40];
    float lat, lon;
    char mqtt_host[40];
    uint16_t mqtt_port;
    char mqtt_user[24];
    char mqtt_pass[24];
    char web_pass[24];
    uint8_t mode, effect, r, g, b, brightness;
    ScheduleEntry schedule[MAX_SCHEDULE];
    uint16_t crc;
};

struct Config {
    uint8_t version;
    uint8_t digits;              // 2..6
    uint8_t hour_format;         // 12 or 24
    uint8_t blank_hour_zero;     // suppress the leading 0 in 12-hour mode

    char tz[48];                 // POSIX TZ, e.g. EST5EDT,M3.2.0,M11.1.0/2
    char ntp_server[40];
    float lat, lon;

    char mqtt_host[40];
    uint16_t mqtt_port;
    char mqtt_user[24];
    char mqtt_pass[24];
    char web_pass[24];           // empty = no auth

    uint8_t mode;                // MODE_*
    uint8_t effect;
    uint8_t r, g, b;
    uint8_t brightness;          // percent, 1..100

    ScheduleEntry schedule[MAX_SCHEDULE];

    // v2 additions. Appended after the schedule so the v1 prefix stays put and
    // migration is a field-by-field copy rather than a reinterpretation.
    uint8_t observe_dst;         // 0 = ignore the rule's DST half, stay on standard time

    // Spend from this before growing the struct again. Adding a field inside
    // reserved space needs no migration and no version bump, which is the whole
    // point of carrying it.
    uint8_t reserved[15];

    uint16_t crc;
};

// Length of the CRC-covered prefix.
//
// NOT sizeof(Config) - sizeof(crc): Config contains a float, so it is 4-byte
// aligned and carries trailing padding after `crc`. Subtracting the field size
// runs the CRC two bytes past crc's offset -- i.e. over the CRC field itself --
// so the value computed on save can never match the one computed on load, and
// every reboot silently falls back to defaults. offsetof is exact regardless of
// padding. (This is not hypothetical; it shipped and was caught on hardware.)
const size_t CONFIG_CRC_LEN = offsetof(Config, crc);
const size_t CONFIG_V1_CRC_LEN = offsetof(ConfigV1, crc);

static inline uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c;
}

// The whole struct must fit the Photon 1's emulated EEPROM, with room to grow.
static_assert(sizeof(Config) <= 2047, "Config exceeds Photon 1 EEPROM");
static_assert(sizeof(ConfigV1) <= 2047, "ConfigV1 exceeds Photon 1 EEPROM");

// Migration is only a field copy if v2 really is v1 plus a tail.
static_assert(offsetof(Config, schedule) == offsetof(ConfigV1, schedule),
              "v2 moved a v1 field; migration must be rewritten");

// Copies a v1 record forward. Field by field rather than a memcpy of the common
// prefix: the static_assert in the header proves the prefix matches today, but
// naming each field means a future reshuffle breaks the build instead of
// silently shifting somebody's MQTT password by two bytes.
static inline bool configMigrateV1(const ConfigV1 &old, Config &out) {
    if (old.version != 1) return false;
    if (crc16((const uint8_t *)&old, CONFIG_V1_CRC_LEN) != old.crc) return false;

    memset(&out, 0, sizeof(out));
    out.version = CFG_VERSION;
    out.digits = old.digits;
    out.hour_format = old.hour_format;
    out.blank_hour_zero = old.blank_hour_zero;
    memcpy(out.tz, old.tz, sizeof(out.tz));
    memcpy(out.ntp_server, old.ntp_server, sizeof(out.ntp_server));
    out.lat = old.lat;
    out.lon = old.lon;
    memcpy(out.mqtt_host, old.mqtt_host, sizeof(out.mqtt_host));
    out.mqtt_port = old.mqtt_port;
    memcpy(out.mqtt_user, old.mqtt_user, sizeof(out.mqtt_user));
    memcpy(out.mqtt_pass, old.mqtt_pass, sizeof(out.mqtt_pass));
    memcpy(out.web_pass, old.web_pass, sizeof(out.web_pass));
    out.mode = old.mode;
    out.effect = old.effect;
    out.r = old.r; out.g = old.g; out.b = old.b;
    out.brightness = old.brightness;
    memcpy(out.schedule, old.schedule, sizeof(out.schedule));

    // v1 clocks were always following whatever their POSIX rule said, so the
    // setting that preserves their behaviour is "on".
    out.observe_dst = 1;
    return true;
}

extern Config cfg;

void configDefaults();
bool configLoad();      // false if EEPROM was invalid and defaults were applied
void configSave();
