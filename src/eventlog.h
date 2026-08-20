#pragma once
#include <stdint.h>
#include <stddef.h>

// A black box that survives a power cut.
//
// When this firmware wedges, MQTT and the Particle cloud are both already gone
// -- they die with the network, which is the very moment worth recording. And
// `retained` SRAM does not survive the power cycle used to recover the device.
// EEPROM survives both, so rare events go here and can be read back afterwards.
//
// Events are rare by construction (Wi-Fi transitions, resets, boots), so the
// flash wear from a whole-struct write per event is not a concern.

enum : uint8_t {
    EV_BOOT        = 1,   // arg = reset reason
    EV_WIFI_DOWN   = 2,
    EV_WIFI_UP     = 3,
    EV_LADDER_KICK = 4,   // ladder tried to re-associate
    EV_RESET_WIFI  = 5,   // about to reset: Wi-Fi down too long
    EV_RESET_QUIET = 6,   // about to reset: no traffic despite Wi-Fi "up"
    EV_MQTT_UP     = 7,
    EV_CLOUD_DOWN  = 8,
    EV_DISPLAY_OFF = 9,
    EV_DISPLAY_ON  = 10,
};

const uint8_t EV_MAX = 40;

struct EvEntry {
    uint32_t uptime;   // seconds since that boot
    uint16_t boot_id;  // increments every boot, so entries group by run
    uint8_t code;
    uint8_t arg;
};

struct EvLog {
    uint16_t magic;
    uint16_t boot_id;
    uint8_t count;     // entries written, capped at EV_MAX
    uint8_t next;      // ring cursor
    uint16_t _pad;
    EvEntry e[EV_MAX];
};

void eventLogBegin(uint8_t resetReason);
void eventLog(uint8_t code, uint8_t arg);
const EvLog &eventLogData();
const char *eventName(uint8_t code);
