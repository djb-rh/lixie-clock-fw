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

    printf("\n--- v1 -> v2 migration ---\n");
    // A firmware update must not cost somebody their timezone, location and
    // broker password, which is what a plain CRC-mismatch reset would do.
    ConfigV1 v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1;
    v1.digits = 6;
    v1.hour_format = 24;
    v1.blank_hour_zero = 0;
    strcpy(v1.tz, "AEST-10AEDT,M10.1.0,M4.1.0/3");
    strcpy(v1.ntp_server, "time.nist.gov");
    v1.lat = -33.8688f; v1.lon = 151.2093f;
    strcpy(v1.mqtt_host, "10.0.0.18");
    v1.mqtt_port = 1883;
    strcpy(v1.mqtt_user, "mqtt");
    strcpy(v1.mqtt_pass, "a-secret-worth-keeping");
    strcpy(v1.web_pass, "hunter2");
    v1.mode = 1; v1.effect = 5;
    v1.r = 12; v1.g = 34; v1.b = 56;
    v1.brightness = 77;
    v1.schedule[0].enabled = 1;
    v1.schedule[0].days_mask = 0x7F;
    v1.schedule[0].anchor = ANCHOR_SUNSET;
    v1.schedule[0].minutes = -45;
    v1.schedule[0].brightness = 15;
    v1.crc = crc16((const uint8_t *)&v1, CONFIG_V1_CRC_LEN);

    Config up;
    check(configMigrateV1(v1, up), "a valid v1 record migrates");
    check(up.version == CFG_VERSION, "version bumped to current");
    check(!strcmp(up.tz, "AEST-10AEDT,M10.1.0,M4.1.0/3"), "timezone preserved");
    check(!strcmp(up.mqtt_pass, "a-secret-worth-keeping"), "broker password preserved");
    check(!strcmp(up.web_pass, "hunter2"), "config password preserved");
    check(up.lat < -33.8f && up.lon > 151.0f, "location preserved");
    check(up.digits == 6 && up.hour_format == 24 && up.brightness == 77,
          "display settings preserved");
    check(up.effect == 5 && up.mode == 1, "effect preserved");
    check(up.schedule[0].enabled == 1 && up.schedule[0].anchor == ANCHOR_SUNSET &&
          up.schedule[0].minutes == -45, "schedule preserved");
    check(up.observe_dst == 1, "DST defaults to on, matching v1 behaviour");

    // And the migrated record must itself round-trip under the v2 CRC.
    up.crc = crc16((const uint8_t *)&up, CONFIG_CRC_LEN);
    Config again = up;
    check(crc16((const uint8_t *)&again, CONFIG_CRC_LEN) == again.crc,
          "migrated record validates under the v2 CRC");

    ConfigV1 corrupt = v1;
    corrupt.lat = 0.0f;                      // CRC no longer matches
    Config nope;
    check(!configMigrateV1(corrupt, nope), "a corrupt v1 record is rejected, not migrated");

    ConfigV1 wrongver = v1;
    wrongver.version = 7;
    wrongver.crc = crc16((const uint8_t *)&wrongver, CONFIG_V1_CRC_LEN);
    check(!configMigrateV1(wrongver, nope), "an unknown version is rejected");

    printf("\n--- reserved space ---\n");
    snprintf(buf, sizeof(buf), "%zu reserved bytes for future fields, no migration needed",
             sizeof(up.reserved));
    check(sizeof(up.reserved) >= 8, buf);

    printf("\n--- schedule capacity ---\n");
    snprintf(buf, sizeof(buf), "%u entries x %zu bytes = %zu bytes of schedule",
             MAX_SCHEDULE, sizeof(ScheduleEntry),
             MAX_SCHEDULE * sizeof(ScheduleEntry));
    check(MAX_SCHEDULE * sizeof(ScheduleEntry) < 1024, buf);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
