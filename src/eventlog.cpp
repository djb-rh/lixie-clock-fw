#include "eventlog.h"
#include "Particle.h"

namespace {
// Well clear of the config struct, which ends around byte 576.
const int EV_ADDR = 1024;
const int ALIVE_ADDR = 1400;        // separate slot: adding it changes no layout
const uint16_t EV_MAGIC = 0x4C58;   // "LX"
const uint32_t HEARTBEAT_MS = 30UL * 60UL * 1000UL;

EvLog g_log;
EvAlive g_alive;
bool g_logDirty = false;
uint32_t g_lastBeat = 0;
}  // namespace

void eventLogBegin(uint8_t resetReason) {
    EEPROM.get(EV_ADDR, g_log);
    if (g_log.magic != EV_MAGIC || g_log.count > EV_MAX || g_log.next >= EV_MAX) {
        memset(&g_log, 0, sizeof(g_log));
        g_log.magic = EV_MAGIC;
    }
    g_log.boot_id++;

    // Read the previous run's last heartbeat BEFORE it gets overwritten -- it is
    // the whole point: it says how far the clock got before it stopped.
    EvAlive prev;
    EEPROM.get(ALIVE_ADDR, prev);
    if (prev.magic == EV_MAGIC) g_alive = prev;
    else { g_alive = EvAlive{}; g_alive.magic = EV_MAGIC; }

    eventLog(EV_BOOT, resetReason);
    EEPROM.put(EV_ADDR, g_log);      // boot record is worth an immediate write
    g_logDirty = false;
}

void eventLog(uint8_t code, uint8_t arg) {
    EvEntry &e = g_log.e[g_log.next];
    e.uptime = millis() / 1000;
    e.boot_id = g_log.boot_id;
    e.code = code;
    e.arg = arg;

    g_log.next = (uint8_t)((g_log.next + 1) % EV_MAX);
    if (g_log.count < EV_MAX) g_log.count++;

    // Deliberately NOT written here.
    //
    // Events are raised from wherever they happen -- including inside the MQTT
    // receive callback, which is the network stack's own call path. An EEPROM
    // write is a flash erase/program that stalls for tens of milliseconds, and
    // doing that from inside a network callback is asking for trouble on a part
    // this small. The write is deferred to eventLogTick() in the main loop,
    // which runs within milliseconds anyway.
    g_logDirty = true;
}

void eventLogTick() {
    if (g_logDirty) {
        g_logDirty = false;
        EEPROM.put(EV_ADDR, g_log);
    }

    uint32_t now = millis();
    if (now - g_lastBeat >= HEARTBEAT_MS) {
        g_lastBeat = now;
        g_alive.magic = EV_MAGIC;
        g_alive.boot_id = g_log.boot_id;
        g_alive.uptime = now / 1000;
        EEPROM.put(ALIVE_ADDR, g_alive);
    }
}

const EvAlive &eventLogAlive() { return g_alive; }

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
        case EV_MQTT_DOWN:   return "mqtt_down";
        case EV_CMD_RX:      return "cmd_rx";
        case EV_RX_STALL:    return "rx_stall";
        default:             return "?";
    }
}
