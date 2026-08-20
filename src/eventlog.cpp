#include "eventlog.h"
#include "Particle.h"

namespace {
// Well clear of the config struct, which ends around byte 576.
const int EV_ADDR = 1024;
const uint16_t EV_MAGIC = 0x4C58;   // "LX"
EvLog g_log;
}  // namespace

void eventLogBegin(uint8_t resetReason) {
    EEPROM.get(EV_ADDR, g_log);
    if (g_log.magic != EV_MAGIC || g_log.count > EV_MAX || g_log.next >= EV_MAX) {
        memset(&g_log, 0, sizeof(g_log));
        g_log.magic = EV_MAGIC;
    }
    g_log.boot_id++;
    eventLog(EV_BOOT, resetReason);
}

void eventLog(uint8_t code, uint8_t arg) {
    EvEntry &e = g_log.e[g_log.next];
    e.uptime = millis() / 1000;
    e.boot_id = g_log.boot_id;
    e.code = code;
    e.arg = arg;

    g_log.next = (uint8_t)((g_log.next + 1) % EV_MAX);
    if (g_log.count < EV_MAX) g_log.count++;

    // Written immediately rather than batched: the whole point is surviving a
    // failure that gives no warning and no chance to flush.
    EEPROM.put(EV_ADDR, g_log);
}

const EvLog &eventLogData() { return g_log; }

const char *eventName(uint8_t code) {
    switch (code) {
        case EV_BOOT:        return "boot";
        case EV_WIFI_DOWN:   return "wifi_down";
        case EV_WIFI_UP:     return "wifi_up";
        case EV_LADDER_KICK: return "ladder_kick";
        case EV_RESET_WIFI:  return "reset_wifi";
        case EV_RESET_QUIET: return "reset_quiet";
        case EV_MQTT_UP:     return "mqtt_up";
        case EV_CLOUD_DOWN:  return "cloud_down";
        case EV_DISPLAY_OFF: return "display_off";
        case EV_DISPLAY_ON:  return "display_on";
        default:             return "?";
    }
}
