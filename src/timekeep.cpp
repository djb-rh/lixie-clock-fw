#include "timekeep.h"
#include "config.h"

// Rolled by hand rather than using the ntp-time library: that one burns a
// software timer thread and cannot report how stale the last sync is, which
// both the status page and the Home Assistant diagnostic sensor need.

namespace {
UDP udp;
TzInfo g_tz;
bool g_tzValid = false;
uint32_t g_lastSyncMs = 0;
bool g_everSynced = false;
uint32_t g_lastAttemptMs = 0;
uint32_t g_retryDelayMs = 15000;
uint32_t g_failedSyncs = 0;

const uint32_t SYNC_INTERVAL_MS = 6UL * 3600UL * 1000UL;   // 6 h
const uint32_t STALE_AFTER_S = 24UL * 3600UL;
const uint32_t NTP_UNIX_DELTA = 2208988800UL;
const uint32_t SANE_MIN_UNIX = 1735689600UL;               // 2025-01-01
}  // namespace

bool Timekeep::syncNow() {
    if (!WiFi.ready() || !cfg.ntp_server[0]) return false;

    IPAddress ip = WiFi.resolve(cfg.ntp_server);
    if (!ip) return false;

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0b11100011;      // LI 3 (unsynchronized), version 4, mode 3 (client)

    if (!udp.begin(0)) return false;
    udp.sendPacket(pkt, sizeof(pkt), ip, 123);

    bool ok = false;
    uint32_t deadline = millis() + 1500;
    while (millis() < deadline) {
        if (udp.parsePacket() >= 48) {
            udp.read(pkt, 48);
            uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                            ((uint32_t)pkt[42] << 8)  | (uint32_t)pkt[43];
            // Reject a server that answered with an unset clock: adopting a 1900
            // or 1970 timestamp is worse than keeping the time we already have.
            if (secs > NTP_UNIX_DELTA && (secs - NTP_UNIX_DELTA) > SANE_MIN_UNIX) {
                Time.setTime((time_t)(secs - NTP_UNIX_DELTA));
                g_lastSyncMs = millis();
                g_everSynced = true;
                ok = true;
            }
            break;
        }
    }
    udp.stop();
    if (!ok) g_failedSyncs++;
    return ok;
}

void Timekeep::begin() {
    g_tzValid = tzParse(cfg.tz, g_tz);
    if (!g_tzValid) {
        // A corrupt rule must not leave the clock silently displaying UTC; fall
        // back to a sane default and let the status page report the problem.
        tzParse("EST5EDT,M3.2.0,M11.1.0/2", g_tz);
    }

    // Applied last so it covers the fallback too. Suppressing the rule's DST
    // half rather than rewriting the stored rule means switching the setting
    // back needs no re-derivation from the browser.
    if (!cfg.observe_dst) g_tz.has_dst = false;
}

void Timekeep::tick() {
    if (!WiFi.ready()) return;
    uint32_t now = millis();

    uint32_t due = g_everSynced ? SYNC_INTERVAL_MS : g_retryDelayMs;
    if (now - g_lastAttemptMs < due) return;

    g_lastAttemptMs = now;
    if (syncNow()) {
        g_retryDelayMs = 15000;
    } else if (!g_everSynced) {
        // Back off so a dead NTP server doesn't hammer the network while the
        // clock is otherwise perfectly usable on Particle cloud time.
        g_retryDelayMs = min(g_retryDelayMs * 2, 300000UL);
    }
}

bool Timekeep::everSynced() { return g_everSynced; }
uint32_t Timekeep::failedSyncs() { return g_failedSyncs; }

uint32_t Timekeep::secondsSinceSync() {
    return g_everSynced ? (millis() - g_lastSyncMs) / 1000 : 0;
}

bool Timekeep::isStale() {
    return !g_everSynced || secondsSinceSync() > STALE_AFTER_S;
}

LocalTime Timekeep::now() { return localFromUtc(g_tz, (int64_t)Time.now()); }

const TzInfo &Timekeep::tz() { return g_tz; }
bool Timekeep::tzValid() { return g_tzValid; }

bool Timekeep::setTz(const char *posix) {
    TzInfo probe;
    if (!tzParse(posix, probe)) return false;
    if (!cfg.observe_dst) probe.has_dst = false;
    g_tz = probe;
    g_tzValid = true;
    strncpy(cfg.tz, posix, sizeof(cfg.tz) - 1);
    cfg.tz[sizeof(cfg.tz) - 1] = 0;
    return true;
}
