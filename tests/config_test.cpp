// Host tests for the persisted-config layout and its CRC invariant.
//
// These exist because of a real bug: the CRC length was written as
// sizeof(Config) - sizeof(crc), which -- thanks to the struct's trailing
// alignment padding -- covered the CRC field itself. Every save produced a
// checksum that no load could reproduce, so every reboot silently reverted to
// defaults while the settings appeared to apply fine at runtime.

#include "../src/config.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main() {
    char buf[200];

    printf("--- layout ---\n");
    snprintf(buf, sizeof(buf), "sizeof(Config) = %zu, fits 2047-byte EEPROM",
             sizeof(Config));
    check(sizeof(Config) <= 2047, buf);

    snprintf(buf, sizeof(buf), "crc at offset %zu, sizeof %zu -> %zu bytes of padding",
             offsetof(Config, crc), sizeof(Config),
             sizeof(Config) - offsetof(Config, crc) - sizeof(uint16_t));
    check(true, buf);

    // The bug in one line: these two lengths are not the same, and using the
    // wrong one silently breaks persistence.
    snprintf(buf, sizeof(buf),
             "CONFIG_CRC_LEN (%zu) is offsetof, not sizeof-minus-field (%zu)",
             CONFIG_CRC_LEN, sizeof(Config) - sizeof(uint16_t));
    check(CONFIG_CRC_LEN == offsetof(Config, crc), buf);

    printf("\n--- CRC round-trip (save then load) ---\n");
    Config a;
    memset(&a, 0, sizeof(a));
    a.version = CFG_VERSION;
    a.digits = 4;
    a.hour_format = 24;
    a.brightness = 42;
    a.r = 0; a.g = 180; a.b = 255;
    a.lat = 35.994f; a.lon = -78.899f;
    strcpy(a.tz, "EST5EDT,M3.2.0,M11.1.0/2");
    strcpy(a.ntp_server, "pool.ntp.org");

    // Save: stamp the CRC.
    a.crc = crc16((const uint8_t *)&a, CONFIG_CRC_LEN);

    // Load: a byte-for-byte copy, as EEPROM.get would produce.
    Config b = a;
    uint16_t recomputed = crc16((const uint8_t *)&b, CONFIG_CRC_LEN);
    snprintf(buf, sizeof(buf), "stored crc 0x%04x == recomputed 0x%04x",
             b.crc, recomputed);
    check(b.crc == recomputed, buf);

    // The same round-trip with the buggy length fails, which is what shipped.
    Config c = a;
    c.crc = crc16((const uint8_t *)&c, sizeof(Config) - sizeof(uint16_t));
    Config d = c;
    uint16_t bad = crc16((const uint8_t *)&d, sizeof(Config) - sizeof(uint16_t));
    snprintf(buf, sizeof(buf),
             "buggy length does NOT round-trip (0x%04x != 0x%04x), as expected",
             d.crc, bad);
    check(d.crc != bad, buf);

    printf("\n--- CRC detects corruption ---\n");
    Config e = a;
    e.brightness ^= 0xFF;
    check(crc16((const uint8_t *)&e, CONFIG_CRC_LEN) != e.crc,
          "flipped brightness byte invalidates the CRC");

    Config f = a;
    f.tz[3] = 'X';
    check(crc16((const uint8_t *)&f, CONFIG_CRC_LEN) != f.crc,
          "corrupted timezone string invalidates the CRC");

    Config g = a;
    check(crc16((const uint8_t *)&g, CONFIG_CRC_LEN) == g.crc,
          "untouched copy still validates");

    printf("\n--- schedule capacity ---\n");
    snprintf(buf, sizeof(buf), "%u entries x %zu bytes = %zu bytes of schedule",
             MAX_SCHEDULE, sizeof(ScheduleEntry),
             MAX_SCHEDULE * sizeof(ScheduleEntry));
    check(MAX_SCHEDULE * sizeof(ScheduleEntry) < 1024, buf);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
