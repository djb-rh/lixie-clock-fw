#pragma once
#include <stddef.h>
#include <stdint.h>

// Persisted settings. Lives in the Photon 1's 2047 bytes of emulated EEPROM as a
// packed struct with a CRC, because Gen 2 has no filesystem.
//
// Deliberately free of any Particle dependency so the layout and CRC invariants
// can be checked on the host -- see tests/config_test.cpp.
//
// Adding a field: bump CFG_VERSION and handle the old layout in configLoad(),
// or accept that existing clocks fall back to defaults on the next boot.

const uint8_t CFG_VERSION = 1;
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

extern Config cfg;

void configDefaults();
bool configLoad();      // false if EEPROM was invalid and defaults were applied
void configSave();
