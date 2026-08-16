/*
 * Lixie Clock firmware -- Phase 1: core
 *
 * Keeps accurate local time on a Particle Photon 1 and renders it, with the
 * network treated as strictly optional. No web UI, effects, schedule or Home
 * Assistant yet -- those are Phases 2 through 5.
 *
 * Configuration in this phase is via Particle functions; Phase 2 replaces them
 * with the real config page. Particle.function/variable names are capped at 12
 * characters on Gen 2 devices.
 *
 * Deliberately a .cpp rather than a .ino: Particle's .ino preprocessor injects
 * generated function prototypes at the top of the file, outside any namespace,
 * which collides with anonymous-namespace statics. A plain .cpp compiles as
 * written, with no source rewriting in between.
 */

#include "Particle.h"

#include "config.h"
#include "display.h"
#include "httpd.h"
#include "netwatch.h"
#include "timekeep.h"

SYSTEM_MODE(SEMI_AUTOMATIC);
SYSTEM_THREAD(ENABLED);

namespace {

const uint32_t SANE_MIN_UNIX = 1735689600UL;   // 2025-01-01; below this, unsynced

ApplicationWatchdog *g_wd;

// Cloud variables double as the plan's out-of-band rescue path for a clock that
// has fallen off the LAN.
int32_t vFreeMem = 0;
int32_t vBootCount = 0;
int32_t vRssi = 0;
char vIp[16] = "0.0.0.0";
char vTime[48] = "unsynced";
char vStatus[220] = "";
// Separate from vTime on purpose: the status refresh owns vTime and rewrites it
// every two seconds, which would clobber a probe result before it can be read.
char vTzProbe[56] = "";

uint32_t g_lastStatus = 0;
uint32_t g_lastRender = 0;

// ~30 fps rather than 50.
//
// NeoPixel::show() disables interrupts for the whole transmission -- about
// 2.4 ms for 80 LEDs at 800 kHz. At 50 fps that is 12% of wall-clock time with
// interrupts off, which is a lot to take away from the Wi-Fi stack for effects
// that are all slow gradients anyway. 33 ms halves it and looks identical.
const uint32_t FRAME_MS = 33;

// A static display still re-renders only when something visible changed: there
// is no reason to bit-bang the strip at all when the colour is not moving.
struct Rendered {
    int32_t stamp = -1;      // hh*3600 + mm*60 + ss, or -1 for "nothing drawn yet"
    uint8_t r = 0, g = 0, b = 0, brightness = 0, digits = 0, effect = 0;
    bool valid = false;
} g_last;

bool timeValid() { return (uint32_t)Time.now() > SANE_MIN_UNIX; }

void renderNow(bool force) {
    if (!timeValid()) {
        // Never show 1969. A single dim amber numeral says "up, waiting for time"
        // without pretending to be a clock.
        if (force || g_last.valid) {
            Display::clear();
            Display::setNumeral(Display::digitCount() - 1, 8, Rgb8{40, 20, 0});
            Display::show(cfg.brightness);
            g_last = Rendered{};
        }
        return;
    }

    LocalTime lt = Timekeep::now();
    int32_t stamp = lt.hour * 3600 + lt.minute * 60 +
                    (cfg.digits >= 6 ? lt.second : 0);

    // FX_SOLID in effect mode is still a static display, so treat it as one.
    const uint8_t fx = (cfg.mode == MODE_EFFECT) ? cfg.effect : (uint8_t)FX_SOLID;
    const bool animated = (fx != FX_SOLID);

    if (!animated && !force && g_last.valid && g_last.stamp == stamp &&
        g_last.r == cfg.r && g_last.g == cfg.g && g_last.b == cfg.b &&
        g_last.brightness == cfg.brightness && g_last.digits == cfg.digits &&
        g_last.effect == fx) {
        return;
    }

    Display::renderClock(lt, fx, Rgb8{cfg.r, cfg.g, cfg.b}, cfg.brightness, millis());
    g_last = Rendered{stamp, cfg.r, cfg.g, cfg.b, cfg.brightness, cfg.digits, fx, true};
}

void updateStatus() {
    vFreeMem = (int32_t)System.freeMemory();
    vRssi = NetWatch::rssi();
    vBootCount = (int32_t)NetWatch::bootCount();

    if (NetWatch::wifiUp()) {
        strncpy(vIp, WiFi.localIP().toString().c_str(), sizeof(vIp) - 1);
        vIp[sizeof(vIp) - 1] = 0;
    }

    if (timeValid()) {
        LocalTime lt = Timekeep::now();
        snprintf(vTime, sizeof(vTime), "%04d-%02d-%02d %02d:%02d:%02d %s",
                 lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second, lt.abbr);
    } else {
        strcpy(vTime, "unsynced");
    }

    snprintf(vStatus, sizeof(vStatus),
             "rgb=%u,%u,%u br=%u fmt=%u dig=%u | "
             "tz=%s ok=%d ntp_age=%lu stale=%d fails=%lu "
             "wifi=%d cloud=%d recov=%lu rst=%d",
             cfg.r, cfg.g, cfg.b, cfg.brightness, cfg.hour_format, cfg.digits,
             cfg.tz, Timekeep::tzValid() ? 1 : 0,
             (unsigned long)Timekeep::secondsSinceSync(),
             Timekeep::isStale() ? 1 : 0, (unsigned long)Timekeep::failedSyncs(),
             NetWatch::wifiUp() ? 1 : 0, NetWatch::cloudUp() ? 1 : 0,
             (unsigned long)NetWatch::wifiRecoveries(), NetWatch::lastResetReason());
}

// --- rescue path ---
//
// The config page owns settings now (Phase 2). What stays here is only what is
// useful when a clock has fallen off the LAN and the web UI is unreachable.

int fnResync(String) { return Timekeep::syncNow() ? 0 : -1; }

int fnReset(String) {
    System.reset();
    return 0;
}

// Preview local time for an arbitrary UTC epoch without waiting for the real
// clock to get there -- this is how a DST transition gets verified in August.
int fnTzAt(String arg) {
    int64_t utc = (int64_t)strtoll(arg.c_str(), nullptr, 10);
    LocalTime lt = localFromUtc(Timekeep::tz(), utc);
    snprintf(vTzProbe, sizeof(vTzProbe), "%04d-%02d-%02d %02d:%02d:%02d %s dst=%d",
             lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second,
             lt.abbr, lt.is_dst ? 1 : 0);
    return lt.hour * 100 + lt.minute;
}

int fnFactory(String) {
    configDefaults();
    configSave();
    System.reset();
    return 0;
}

}  // namespace

void setup() {
    NetWatch::begin();
    configLoad();

    Display::begin();
    Display::setDigitCount(cfg.digits);
    Timekeep::begin();

    Particle.variable("freemem", vFreeMem);
    Particle.variable("bootcount", vBootCount);
    Particle.variable("rssi", vRssi);
    Particle.variable("ip", vIp, STRING);
    Particle.variable("time", vTime, STRING);
    Particle.variable("status", vStatus, STRING);
    Particle.variable("tzprobe", vTzProbe, STRING);

    Particle.function("resync", fnResync);
    Particle.function("reset", fnReset);
    Particle.function("tzat", fnTzAt);
    Particle.function("factory", fnFactory);

    Httpd::begin();

    WiFi.on();
    WiFi.connect();
    Particle.connect();

    renderNow(true);

    // 120 s, and loop() checks in every pass. SYSTEM_THREAD keeps loop running
    // during an OTA, so this cannot race a firmware update on a clock nobody is
    // standing next to.
    g_wd = new ApplicationWatchdog(120000, System.reset, 1536);
}

void loop() {
    uint32_t now = millis();

    NetWatch::tick();
    Timekeep::tick();
    if (NetWatch::wifiUp()) Httpd::tick();

    if (now - g_lastRender >= FRAME_MS) {
        g_lastRender = now;
        renderNow(false);
    }

    if (now - g_lastStatus >= 2000) {
        g_lastStatus = now;
        updateStatus();
    }

    ApplicationWatchdog::checkin();
}
