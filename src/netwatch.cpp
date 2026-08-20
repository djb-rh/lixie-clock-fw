#include "netwatch.h"

#include "timekeep.h"

// Backup-SRAM counters survive a reset, so a clock that is quietly rebooting
// every hour becomes visible on the status page instead of mysterious.
//
// These are NOT zero-initialized: backup SRAM keeps whatever the previous
// firmware left at the same address. Without the magic guard below, flashing a
// build with a different retained layout silently inherits another variable's
// value as a counter -- which is exactly how this shipped reading 70 Wi-Fi
// recoveries thirty seconds after its first boot.
retained uint32_t g_retainedMagic;
retained uint32_t g_bootCount;
retained uint32_t g_wifiRecoveries;

// Bump whenever the retained block's layout changes.
const uint32_t RETAINED_MAGIC = 0x4C495831;   // "LIX1"

namespace {
const uint32_t CHECK_INTERVAL_MS = 10000;
const uint32_t WIFI_RECONNECT_AFTER_S = 60;
const uint32_t WIFI_REBOOT_AFTER_S = 180;
const uint32_t CLOUD_RECONNECT_AFTER_S = 600;
const uint32_t RSSI_INTERVAL_MS = 30000;

// WiFi.ready() reporting true is not proof of connectivity -- the interface can
// sit associated-but-useless, in which case nothing above ever escalates.
//
// The backstop must PROBE rather than wait, or it punishes a perfectly healthy
// clock: with no broker configured and the Particle cloud unreachable, the only
// other traffic is an NTP sync every six hours, and a passive timer would
// reboot such a clock every quarter of an hour forever. So after a few minutes
// of quiet we force an NTP round trip -- which is both a real connectivity test
// and, on success, proof of life. Only sustained failure of that probe reboots.
const uint32_t QUIET_PROBE_AFTER_S = 300;
const uint32_t NO_TRAFFIC_RESET_AFTER_S = 900;

uint32_t g_lastCheck = 0;
uint32_t g_wifiDownSince = 0;      // millis, 0 when up
uint32_t g_cloudDownSince = 0;
uint32_t g_lastRadioKick = 0;
uint32_t g_lastRssi = 0;
uint32_t g_lastAlive = 0;
uint32_t g_lastProbe = 0;
int g_rssi = 0;
int g_resetReason = 0;

uint32_t secondsSince(uint32_t since) {
    return since ? (millis() - since) / 1000 : 0;
}
}  // namespace

void NetWatch::begin() {
    if (g_retainedMagic != RETAINED_MAGIC) {
        g_retainedMagic = RETAINED_MAGIC;
        g_bootCount = 0;
        g_wifiRecoveries = 0;
    }
    g_bootCount++;
    g_resetReason = System.resetReason();
    g_wifiDownSince = millis();
    g_cloudDownSince = millis();
    g_lastAlive = millis();
}

void NetWatch::tick() {
    uint32_t now = millis();

    if (now - g_lastRssi >= RSSI_INTERVAL_MS && WiFi.ready()) {
        g_lastRssi = now;
        g_rssi = (int)WiFi.RSSI();
    }

    if (now - g_lastCheck < CHECK_INTERVAL_MS) return;
    g_lastCheck = now;

    if (WiFi.ready()) {
        g_wifiDownSince = 0;
        if (Particle.connected()) g_lastAlive = now;

        uint32_t quiet = (now - g_lastAlive) / 1000;
        if (quiet > NO_TRAFFIC_RESET_AFTER_S) {
            System.reset();
        } else if (quiet > QUIET_PROBE_AFTER_S &&
                   now - g_lastProbe > QUIET_PROBE_AFTER_S * 1000UL) {
            g_lastProbe = now;
            Timekeep::syncNow();      // updates g_lastAlive itself on success
        }
    } else {
        if (!g_wifiDownSince) g_wifiDownSince = now;
        uint32_t down = secondsSince(g_wifiDownSince);

        if (down > WIFI_REBOOT_AFTER_S) {
            // Rebooting is the remedy, not the last resort. It takes about ten
            // seconds and it always works; the alternative below used to be
            // cleverer and was the single worst bug in this firmware.
            System.reset();
        } else if (down > WIFI_RECONNECT_AFTER_S &&
                   now - g_lastRadioKick > WIFI_RECONNECT_AFTER_S * 1000UL) {
            g_lastRadioKick = now;
            g_wifiRecoveries++;

            // NEVER WiFi.off()/WiFi.on() here.
            //
            // This used to power-cycle the radio, and on Gen 2 that deadlocks
            // the device: the app thread halts inside the call, the display
            // freezes mid-frame, the status LED sticks at solid white (breathing
            // white -- "Wi-Fi off" -- frozen mid-breath), and the application
            // watchdog cannot rescue it because System.reset() blocks on the
            // same lock. The only way out is pulling power.
            //
            // It is a race, not a certainty: the same call survived two
            // deliberate outage tests on one clock and bricked the other within
            // 95 minutes of a routine disassociation. A plain re-associate does
            // the same job with none of the risk, and if it does not work the
            // reset above handles it.
            WiFi.connect();
        }
        return;   // no point checking the cloud with no network under it
    }

    if (Particle.connected()) {
        g_cloudDownSince = 0;
    } else {
        if (!g_cloudDownSince) g_cloudDownSince = now;
        if (secondsSince(g_cloudDownSince) > CLOUD_RECONNECT_AFTER_S) {
            // Cloud is for OTA only, so this is a convenience retry, never a
            // reason to reboot a clock that is otherwise working fine.
            g_cloudDownSince = now;
            Particle.disconnect();
            Particle.connect();
        }
    }
}

bool NetWatch::wifiUp() { return WiFi.ready(); }
bool NetWatch::cloudUp() { return Particle.connected(); }
uint32_t NetWatch::wifiDownSeconds() { return secondsSince(g_wifiDownSince); }
int NetWatch::rssi() { return g_rssi; }
uint32_t NetWatch::bootCount() { return g_bootCount; }
int NetWatch::lastResetReason() { return g_resetReason; }
uint32_t NetWatch::wifiRecoveries() { return g_wifiRecoveries; }
void NetWatch::noteAlive() { g_lastAlive = millis(); }
uint32_t NetWatch::secondsSinceTraffic() { return (millis() - g_lastAlive) / 1000; }
