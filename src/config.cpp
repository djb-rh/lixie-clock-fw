#include "config.h"
#include "Particle.h"

Config cfg;

void configDefaults() {
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = CFG_VERSION;
    cfg.digits = 4;
    cfg.hour_format = 12;
    cfg.blank_hour_zero = 1;
    strcpy(cfg.tz, "EST5EDT,M3.2.0,M11.1.0/2");
    strcpy(cfg.ntp_server, "pool.ntp.org");
    cfg.lat = 35.994f;
    cfg.lon = -78.899f;
    cfg.mqtt_port = 1883;
    cfg.mode = MODE_SOLID;
    cfg.r = 255; cfg.g = 136; cfg.b = 0;
    cfg.brightness = 60;
}

bool configLoad() {
    EEPROM.get(0, cfg);
    uint16_t want = crc16((const uint8_t *)&cfg, CONFIG_CRC_LEN);
    if (cfg.version != CFG_VERSION || cfg.crc != want) {
        configDefaults();
        return false;
    }

    // Defend against a valid-CRC struct that would still render nothing.
    if (cfg.digits < 2 || cfg.digits > MAX_DIGITS) cfg.digits = 4;
    if (cfg.brightness < 1 || cfg.brightness > 100) cfg.brightness = 60;
    if (cfg.hour_format != 12 && cfg.hour_format != 24) cfg.hour_format = 12;
    if (cfg.mqtt_port == 0) cfg.mqtt_port = 1883;
    cfg.tz[sizeof(cfg.tz) - 1] = 0;
    cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = 0;
    return true;
}

void configSave() {
    cfg.crc = crc16((const uint8_t *)&cfg, CONFIG_CRC_LEN);

    // EEPROM emulation writes to flash, so only touch it when something actually
    // changed -- these clocks are expected to run for years. Compare the
    // meaningful prefix and the CRC only; trailing padding is indeterminate and
    // would cause pointless writes.
    Config current;
    EEPROM.get(0, current);
    if (memcmp(&current, &cfg, CONFIG_CRC_LEN) == 0 && current.crc == cfg.crc) return;

    EEPROM.put(0, cfg);
}
